// SPDX-License-Identifier: MIT
// Copyright (c) 2024 NVLink Placement Contributors

#include "broker_server.h"
#include "brk2_protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <limits>
#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace nvlink::broker {
namespace {

using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;

bool read_exact(SocketHandle socket, void* buffer, std::size_t size) {
    uint8_t* out = static_cast<uint8_t*>(buffer);
    std::size_t total = 0;
    while (total < size) {
        const ssize_t received = ::recv(socket, out + total, size - total, 0);
        if (received == 0) {
            return false;
        }
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return false;
            }
            return false;
        }
        total += static_cast<std::size_t>(received);
    }
    return true;
}

bool read_exact_until(SocketHandle socket, void* buffer, std::size_t size,
                      std::chrono::steady_clock::time_point deadline) {
    uint8_t* out = static_cast<uint8_t*>(buffer);
    std::size_t total = 0;
    while (total < size) {
        const auto remaining = deadline - std::chrono::steady_clock::now();
        if (remaining <= std::chrono::steady_clock::duration::zero()) return false;
        const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count();
        pollfd pfd{};
        pfd.fd = socket;
        pfd.events = POLLIN;
        const int poll_ms = static_cast<int>(std::min<long long>(millis + 1, 60000));
        const int ready = ::poll(&pfd, 1, poll_ms);
        if (ready < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (ready == 0 || (pfd.revents & (POLLERR | POLLNVAL)) != 0) return false;
        const ssize_t received = ::recv(socket, out + total, size - total, 0);
        if (received == 0) return false;
        if (received < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return false;
        }
        total += static_cast<std::size_t>(received);
    }
    return true;
}

bool write_exact(SocketHandle socket, const void* buffer, std::size_t size) {
    const uint8_t* in = static_cast<const uint8_t*>(buffer);
    std::size_t total = 0;
    while (total < size) {
        const ssize_t written = ::send(socket, in + total, size - total, MSG_NOSIGNAL);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return false;
            }
            return false;
        }
        if (written == 0) {
            return false;
        }
        total += static_cast<std::size_t>(written);
    }
    return true;
}

void send_brk2_protocol_error(SocketHandle socket, const std::string& task_id,
                                uint64_t request_id, const std::string& error,
                                const ProtocolLimits& limits) {
    BrokerResponse response;
    response.task_id = task_id;
    response.status = TaskStatus::Rejected;
    response.error = error;
    response.selected_gpu = -1;
    const auto body = encode_brk2_response_body(response, request_id, limits);
    const uint32_t length_be = htonl(static_cast<uint32_t>(body.size()));
    (void)write_exact(socket, &length_be, sizeof(length_be));
    (void)write_exact(socket, body.data(), body.size());
}

void close_socket(SocketHandle socket) noexcept {
    if (socket != kInvalidSocket) {
        ::close(socket);
    }
}

void shutdown_socket(SocketHandle socket) noexcept {
    if (socket != kInvalidSocket) {
        ::shutdown(socket, SHUT_RDWR);
    }
}

void set_socket_timeout(SocketHandle socket, int timeout_ms) {
    timeval timeout{};
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;
    if (::setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
        ::setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0) {
        throw std::runtime_error(std::string("failed to configure socket timeout: ") + std::strerror(errno));
    }
}

void send_protocol_error(SocketHandle socket,
                         const std::string& task_id,
                         const std::string& error,
                         const ProtocolLimits& limits) {
    BrokerResponse response;
    response.task_id = task_id;
    response.status = TaskStatus::Rejected;
    response.error = error;
    response.selected_gpu = -1;

    const std::vector<uint8_t> frame = encode_response_frame(response, limits);
    const uint32_t frame_len = static_cast<uint32_t>(frame.size());
    const uint32_t frame_len_be = htonl(frame_len);
    (void)write_exact(socket, &frame_len_be, sizeof(frame_len_be));
    (void)write_exact(socket, frame.data(), frame.size());
}

} // namespace

class BrokerServer::Impl {
public:
    explicit Impl(const BrokerServerConfig& config)
        : config_(config), service_(config.service_config) {
        if (config_.port < 1 || config_.port > 65535) {
            throw std::invalid_argument("broker server port must be in range 1..65535");
        }
        if (config_.max_inflight_per_client == 0 || config_.max_clients == 0) {
            throw std::invalid_argument("client and in-flight limits must be greater than zero");
        }
        if (config_.socket_timeout_ms <= 0) {
            throw std::invalid_argument("socket_timeout_ms must be greater than zero");
        }
        if (config_.max_inflight_memory_bytes == 0) {
            throw std::invalid_argument("max_inflight_memory_bytes must be greater than zero");
        }
        constexpr std::size_t kMaxOutstandingRequests = 65536;
        const std::size_t derived_limit =
            config_.max_clients > kMaxOutstandingRequests / config_.max_inflight_per_client
                ? kMaxOutstandingRequests
                : config_.max_clients * config_.max_inflight_per_client;
        if (config_.max_outstanding_requests > kMaxOutstandingRequests) {
            throw std::invalid_argument("max_outstanding_requests must not exceed 65536");
        }
        const std::size_t request_limit = config_.max_outstanding_requests == 0
            ? derived_limit
            : std::min(config_.max_outstanding_requests, derived_limit);
        admission_budget_ = std::make_shared<WorkerAdmissionBudget>(
            request_limit, config_.max_inflight_memory_bytes);

        const std::size_t hw = std::max<std::size_t>(1, std::thread::hardware_concurrency());
        const std::size_t automatic_total = std::min<std::size_t>(256, std::max<std::size_t>(5, hw * 2));
        const std::array<std::size_t, 4> reserved_counts{{
            config_.control_worker_threads,
            config_.realtime_spatial_worker_threads,
            config_.exact_cpu_worker_threads,
            config_.inference_worker_threads
        }};
        std::size_t reserved = 0;
        for (const std::size_t count : reserved_counts) {
            if (count == 0 || count > 255 - reserved) {
                throw std::invalid_argument(
                    "control, realtime, exact CPU and inference pools each need at least one worker; total must be below 256");
            }
            reserved += count;
        }
        constexpr std::size_t kMaximumElasticWorkers = 64;
        const std::size_t elastic_workers = config_.elastic_worker_threads;
        if (elastic_workers > kMaximumElasticWorkers ||
            elastic_workers > 256 - reserved) {
            throw std::invalid_argument("elastic worker count must be in 0..64 and fit the 256 worker process cap");
        }

        std::size_t throughput_workers = config_.throughput_worker_threads;
        if (throughput_workers == 0) {
            const std::size_t total_budget = config_.worker_threads == 0
                ? automatic_total
                : std::max<std::size_t>(5, config_.worker_threads);
            const std::size_t required = reserved + elastic_workers;
            if (config_.worker_threads != 0 && total_budget <= required) {
                throw std::invalid_argument(
                    "--workers TOTAL must leave at least one throughput worker after reserved and elastic workers");
            }
            throughput_workers = total_budget > required ? total_budget - required : 1;
        }
        if (throughput_workers == 0 ||
            throughput_workers > 256 - reserved - elastic_workers) {
            throw std::invalid_argument("dedicated plus elastic worker total must be in 1..256");
        }
        WorkerPoolConfig pool_config;
        pool_config.workers = {{
            config_.control_worker_threads,
            config_.realtime_spatial_worker_threads,
            throughput_workers,
            config_.exact_cpu_worker_threads,
            config_.inference_worker_threads
        }};
        pool_config.elastic_workers = elastic_workers;
        pool_config.elastic_max_per_class = config_.elastic_max_workers_per_class;
        worker_pool_ = std::make_unique<ClassWorkerPool>(pool_config);
    }

    ~Impl() {
        stop();
        join_all();
    }

    void run() {
        bool expected = false;
        if (!running_.compare_exchange_strong(expected, true)) {
            throw std::runtime_error("broker server is already running");
        }

        try {
            open_listener();
            accept_loop();
        } catch (...) {
            stop();
            join_all();
            throw;
        }

        join_all();
    }

    void stop() noexcept {
        const bool was_running = running_.exchange(false);
        if (was_running) {
            shutdown_socket(listener_socket_);
            close_socket(listener_socket_);
            listener_socket_ = kInvalidSocket;

            std::lock_guard<std::mutex> lock(client_mutex_);
            for (SocketHandle socket : client_sockets_) {
                shutdown_socket(socket);
            }
        }
        if (worker_pool_) worker_pool_->stop();
    }

    bool is_running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

private:
    struct SessionState {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };

    struct SessionInflight {
        std::mutex mutex;
        std::condition_variable cv;
        std::size_t count = 0;

        void finish() noexcept {
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (count > 0) --count;
            }
            cv.notify_one();
        }
    };

    struct InflightCompletion {
        std::shared_ptr<SessionInflight> state;
        std::shared_ptr<WorkerAdmissionBudget> budget;
        std::size_t payload_bytes = 0;
        ~InflightCompletion() {
            if (state) state->finish();
            if (budget) budget->release(payload_bytes);
        }
    };

    bool enqueue_task(WorkerClass worker_class,
                      std::optional<uint64_t> spatial_affinity_key,
                      std::optional<uint32_t> preferred_numa_node,
                      std::optional<std::chrono::steady_clock::time_point> deadline,
                      ClassWorkerPool::Task task) {
        return worker_pool_ && worker_pool_->enqueue_with_deadline(
            worker_class, spatial_affinity_key, preferred_numa_node, deadline, std::move(task));
    }

    void open_listener() {
        listener_socket_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listener_socket_ == kInvalidSocket) {
            throw std::runtime_error("failed to create broker listener socket");
        }

        const int reuse = 1;
        if (::setsockopt(listener_socket_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0) {
            const int saved_errno = errno;
            close_socket(listener_socket_);
            listener_socket_ = kInvalidSocket;
            throw std::runtime_error(std::string("failed to set SO_REUSEADDR: ") + std::strerror(saved_errno));
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(config_.port));
        addr.sin_addr.s_addr = htonl(INADDR_ANY);

        if (::bind(listener_socket_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            const int saved_errno = errno;
            close_socket(listener_socket_);
            listener_socket_ = kInvalidSocket;
            throw std::runtime_error(std::string("failed to bind broker listener: ") + std::strerror(saved_errno));
        }

        if (::listen(listener_socket_, SOMAXCONN) != 0) {
            const int saved_errno = errno;
            close_socket(listener_socket_);
            listener_socket_ = kInvalidSocket;
            throw std::runtime_error(std::string("failed to listen on broker listener: ") + std::strerror(saved_errno));
        }
    }

    void accept_loop() {
        while (running_.load(std::memory_order_acquire)) {
            reap_finished_sessions(false);

            sockaddr_in client_addr{};
            socklen_t client_len = sizeof(client_addr);
            SocketHandle client_socket = ::accept(listener_socket_, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
            if (client_socket == kInvalidSocket) {
                if (!running_.load(std::memory_order_acquire)) {
                    break;
                }
                if (errno == EINTR) {
                    continue;
                }
                throw std::runtime_error(std::string("accept failed: ") + std::strerror(errno));
            }

            if (!admit_client(client_socket)) {
                close_socket(client_socket);
                continue;
            }

            auto done = std::make_shared<std::atomic<bool>>(false);
            session_threads_.push_back(SessionState{
                std::thread([this, client_socket, done] {
                    session_loop(client_socket);
                    done->store(true, std::memory_order_release);
                }),
                done,
            });
        }
    }

    bool admit_client(SocketHandle client_socket) {
        std::lock_guard<std::mutex> lock(client_mutex_);
        if (client_sockets_.size() >= config_.max_clients) {
            send_protocol_error(client_socket,
                                "",
                                "too many concurrent broker clients",
                                config_.service_config.protocol_limits);
            shutdown_socket(client_socket);
            return false;
        }

        try {
            set_socket_timeout(client_socket, config_.socket_timeout_ms);
        } catch (const std::exception&) {
            shutdown_socket(client_socket);
            return false;
        }

        client_sockets_.insert(client_socket);
        return true;
    }

    void session_loop(SocketHandle client_socket) {
        auto inflight = std::make_shared<SessionInflight>();
        std::mutex write_mutex;
        auto writes_enabled = std::make_shared<std::atomic<bool>>(true);

        auto write_response = [this, client_socket, &write_mutex, writes_enabled]
            (const BrokerResponse& response, bool use_brk2, uint64_t request_id) {
            if (!writes_enabled->load(std::memory_order_acquire)) return false;
            const auto response_frame = use_brk2
                ? encode_brk2_response_body(response, request_id, config_.service_config.protocol_limits)
                : encode_response_frame(response, config_.service_config.protocol_limits);
            const uint32_t len_be = htonl(static_cast<uint32_t>(response_frame.size()));
            std::lock_guard<std::mutex> lock(write_mutex);
            if (!writes_enabled->load(std::memory_order_acquire)) return false;
            return write_exact(client_socket, &len_be, sizeof(len_be)) &&
                   write_exact(client_socket, response_frame.data(), response_frame.size());
        };

        auto send_error = [this, client_socket, &write_mutex, writes_enabled]
            (const std::string& task_id, const std::string& error, bool use_brk2, uint64_t request_id) {
            std::lock_guard<std::mutex> lock(write_mutex);
            if (!writes_enabled->load(std::memory_order_acquire)) return;
            if (use_brk2) {
                send_brk2_protocol_error(client_socket, task_id, request_id, error,
                                          config_.service_config.protocol_limits);
            } else {
                send_protocol_error(client_socket, task_id, error,
                                    config_.service_config.protocol_limits);
            }
        };

        try {
            while (running_.load(std::memory_order_acquire)) {
                // The idle timeout only waits for the first prefix byte. Once a
                // frame starts, its prefix and body share one absolute deadline;
                // slow byte drips cannot extend the deadline by resetting SO_RCVTIMEO.
                uint8_t prefix[sizeof(uint32_t)]{};
                if (!read_exact(client_socket, prefix, 1)) break;
                const auto frame_deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(config_.socket_timeout_ms);
                if (!read_exact_until(client_socket, prefix + 1, sizeof(prefix) - 1, frame_deadline)) break;
                uint32_t frame_len_be = 0;
                std::memcpy(&frame_len_be, prefix, sizeof(frame_len_be));
                const std::size_t frame_len = ntohl(frame_len_be);
                if (frame_len == 0 || frame_len > config_.service_config.protocol_limits.max_frame_bytes) {
                    send_error("", "frame length violates protocol limits", false, 0);
                    break;
                }

                std::vector<uint8_t> frame(frame_len);
                if (!read_exact_until(client_socket, frame.data(), frame.size(), frame_deadline)) break;

                const bool use_brk2 = frame.size() >= 4 &&
                    frame[0] == 'B' && frame[1] == 'R' && frame[2] == 'K' && frame[3] == '2';
                BrokerRequest request;
                std::string decode_error;
                uint64_t request_id = 0;
                bool decoded = false;
                if (use_brk2) {
                    if (frame.size() >= 20) {
                        for (std::size_t i = 12; i < 20; ++i)
                            request_id = (request_id << 8) | frame[i];
                    }
                    decoded = decode_brk2_request_body(frame, &request, &request_id,
                        &decode_error, config_.service_config.protocol_limits);
                } else {
                    decoded = decode_request_frame(frame, &request, &decode_error,
                        config_.service_config.protocol_limits);
                }
                if (!decoded) {
                    send_error(request.task_id, decode_error, use_brk2, request_id);
                    continue;
                }

                const std::string rejected_task_id = request.task_id;
                const auto admitted_at = std::chrono::steady_clock::now();
                auto deadline = admitted_at + std::chrono::milliseconds(request.timeout_ms);
                if (request.deadline_unix_ns) {
                    const auto max_ns = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
                    if (*request.deadline_unix_ns <= max_ns) {
                        const auto wall_deadline = std::chrono::system_clock::time_point(
                            std::chrono::nanoseconds(static_cast<int64_t>(*request.deadline_unix_ns)));
                        const auto remaining = wall_deadline - std::chrono::system_clock::now();
                        const auto translated = remaining <= decltype(remaining)::zero()
                            ? admitted_at
                            : admitted_at + std::chrono::duration_cast<std::chrono::steady_clock::duration>(remaining);
                        deadline = std::min(deadline, translated);
                    } else {
                        send_error(rejected_task_id, "BRK2 absolute deadline is out of range", use_brk2, request_id);
                        continue;
                    }
                }

                const std::size_t payload_bytes = request.payload.size() +
                    request.task_id.size() + request.source.size() +
                    request.destination.size() + request.kind.size() +
                    sizeof(BrokerRequest) + 256;
                // Allocate the RAII guard before taking reservations so even an
                // allocation failure cannot strand per-client/global capacity.
                auto completion = std::make_shared<InflightCompletion>();
                if (!admission_budget_ || !admission_budget_->try_acquire(payload_bytes)) {
                    send_error(rejected_task_id, "broker outstanding-work budget exhausted", use_brk2, request_id);
                    continue;
                }
                completion->budget = admission_budget_;
                completion->payload_bytes = payload_bytes;

                bool admitted = false;
                {
                    std::lock_guard<std::mutex> lock(inflight->mutex);
                    if (inflight->count < config_.max_inflight_per_client) {
                        ++inflight->count;
                        admitted = true;
                    }
                }
                if (!admitted) {
                    completion.reset();
                    send_error(rejected_task_id, "too many inflight requests for client",
                               use_brk2, request_id);
                    continue;
                }
                completion->state = inflight;
                const WorkerClass worker_class = request.worker_class;
                const auto affinity_key = request.spatial_affinity_key;
                const auto preferred_node = request.preferred_numa_node;
                const bool queued = enqueue_task(
                    worker_class, affinity_key, preferred_node, deadline,
                    [this, request = std::move(request), write_response, completion,
                     use_brk2, request_id, admitted_at, deadline]
                    (const WorkerExecutionContext& context) mutable {
                        try {
                            const auto service_started = std::chrono::steady_clock::now();
                            BrokerResponse response;
                            if (service_started >= deadline) {
                                response.task_id = request.task_id;
                                response.status = TaskStatus::Timeout;
                                response.error = "request deadline expired in worker queue";
                            } else {
                                const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(
                                    deadline - service_started).count();
                                request.timeout_ms = static_cast<uint32_t>(std::max<int64_t>(
                                    1, std::min<int64_t>(request.timeout_ms, remaining)));
                                response = service_.handle_request(request);
                            }
                            const auto service_ended = std::chrono::steady_clock::now();
                            if (service_ended >= deadline) {
                                response.status = TaskStatus::Timeout;
                                response.result.clear();
                                response.error = "request deadline exceeded";
                            }
                            response.worker_class = context.worker_class;
                            response.worker_telemetry_available = true;
                            response.queue_wait_us = context.queue_wait_us;
                            const auto service_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                service_ended - service_started).count();
                            response.service_time_us = service_us > 0 ? static_cast<uint64_t>(service_us) : 0;
                            response.worker_id = context.worker_id;
                            response.worker_numa_node = context.numa_node;
                            response.latency_ms = static_cast<uint64_t>(
                                std::chrono::duration_cast<std::chrono::milliseconds>(
                                    service_ended - admitted_at).count());
                            (void)write_response(response, use_brk2, request_id);
                        } catch (...) {
                            // Completion guard releases the per-session slot on every exit.
                        }
                    });
                if (!queued) {
                    completion.reset();
                    send_error(rejected_task_id,
                               "worker queue could not accept task (shutdown or memory allocation failure)",
                               use_brk2, request_id);
                    break;
                }
            }
        } catch (...) {
            // Socket/session failures are isolated to this client.
        }

        writes_enabled->store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(client_mutex_);
            client_sockets_.erase(client_socket);
        }
        shutdown_socket(client_socket);
        {
            std::unique_lock<std::mutex> lock(inflight->mutex);
            inflight->cv.wait(lock, [&inflight] { return inflight->count == 0; });
        }
        close_socket(client_socket);
    }

    void reap_finished_sessions(bool join_all) {
        auto it = session_threads_.begin();
        while (it != session_threads_.end()) {
            const bool finished = it->done != nullptr && it->done->load(std::memory_order_acquire);
            if (join_all || finished) {
                if (it->thread.joinable()) {
                    it->thread.join();
                }
                it = session_threads_.erase(it);
            } else {
                ++it;
            }
        }
    }

    void join_all() noexcept {
        shutdown_socket(listener_socket_);
        close_socket(listener_socket_);
        listener_socket_ = kInvalidSocket;
        reap_finished_sessions(true);
        if (worker_pool_) { worker_pool_->stop(); worker_pool_->join(); }
    }

    BrokerServerConfig config_;
    BrokerService service_;
    std::atomic<bool> running_{false};
    SocketHandle listener_socket_ = kInvalidSocket;
    std::unique_ptr<ClassWorkerPool> worker_pool_;
    std::shared_ptr<WorkerAdmissionBudget> admission_budget_;
    std::vector<SessionState> session_threads_;
    std::unordered_set<SocketHandle> client_sockets_;
    mutable std::mutex client_mutex_;
};

BrokerServer::BrokerServer(const BrokerServerConfig& config)
    : pimpl_(std::make_unique<Impl>(config)) {}

BrokerServer::~BrokerServer() = default;

void BrokerServer::run() {
    pimpl_->run();
}

void BrokerServer::stop() noexcept {
    pimpl_->stop();
}

bool BrokerServer::is_running() const noexcept {
    return pimpl_->is_running();
}

} // namespace nvlink::broker
