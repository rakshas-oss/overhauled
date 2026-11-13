// SPDX-License-Identifier: MIT
// Copyright (c) 2024 NVLink Placement Contributors

#include "broker_server.h"
#include "brk2_protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
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

void send_brk2_protocol_error(SocketHandle socket, uint64_t request_id,
                                const std::string& error, const ProtocolLimits& limits) {
    BrokerResponse response;
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
        const std::size_t hw = std::max<std::size_t>(1, std::thread::hardware_concurrency());
        const std::size_t pool_size = config_.worker_threads == 0
            ? std::min<std::size_t>(256, std::max<std::size_t>(4, hw * 2))
            : config_.worker_threads;
        if (pool_size == 0 || pool_size > 256) {
            throw std::invalid_argument("worker_threads must be between 1 and 256 (or zero for auto)");
        }
        const std::size_t requested_queue = config_.max_clients >
            (65536 / config_.max_inflight_per_client)
            ? 65536
            : config_.max_clients * config_.max_inflight_per_client;
        max_queued_tasks_ = std::max<std::size_t>(64, std::min<std::size_t>(65536, requested_queue));

        try {
            workers_.reserve(pool_size);
            for (std::size_t i = 0; i < pool_size; ++i) {
                workers_.emplace_back([this] { worker_loop(); });
            }
        } catch (...) {
            {
                std::lock_guard<std::mutex> lock(queue_mutex_);
                workers_stopping_ = true;
            }
            queue_cv_.notify_all();
            for (auto& worker : workers_) {
                if (worker.joinable()) worker.join();
            }
            throw;
        }
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
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            workers_stopping_ = true;
        }
        queue_cv_.notify_all();
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
        ~InflightCompletion() { if (state) state->finish(); }
    };

    void worker_loop() noexcept {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(queue_mutex_);
                queue_cv_.wait(lock, [this] { return workers_stopping_ || !task_queue_.empty(); });
                if (workers_stopping_ && task_queue_.empty()) return;
                task = std::move(task_queue_.front());
                task_queue_.pop_front();
            }
            try { if (task) task(); } catch (...) {
                // A task must never terminate the worker pool.
            }
        }
    }

    bool enqueue_task(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (workers_stopping_ || task_queue_.size() >= max_queued_tasks_) return false;
            task_queue_.push_back(std::move(task));
        }
        queue_cv_.notify_one();
        return true;
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
                send_brk2_protocol_error(client_socket, request_id, error,
                                          config_.service_config.protocol_limits);
            } else {
                send_protocol_error(client_socket, task_id, error,
                                    config_.service_config.protocol_limits);
            }
        };

        try {
            while (running_.load(std::memory_order_acquire)) {
                uint32_t frame_len_be = 0;
                if (!read_exact(client_socket, &frame_len_be, sizeof(frame_len_be))) break;
                const std::size_t frame_len = ntohl(frame_len_be);
                if (frame_len == 0 || frame_len > config_.service_config.protocol_limits.max_frame_bytes) {
                    send_error("", "frame length violates protocol limits", false, 0);
                    break;
                }

                std::vector<uint8_t> frame(frame_len);
                if (!read_exact(client_socket, frame.data(), frame.size())) break;

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

                bool admitted = false;
                {
                    std::lock_guard<std::mutex> lock(inflight->mutex);
                    if (inflight->count < config_.max_inflight_per_client) {
                        ++inflight->count;
                        admitted = true;
                    }
                }
                if (!admitted) {
                    send_error(request.task_id, "too many inflight requests for client",
                               use_brk2, request_id);
                    continue;
                }

                const std::string rejected_task_id = request.task_id;
                auto completion = std::make_shared<InflightCompletion>();
                completion->state = inflight;
                const bool queued = enqueue_task(
                    [this, request = std::move(request), write_response, completion,
                     use_brk2, request_id]() mutable {
                        try {
                            BrokerResponse response = service_.handle_request(request);
                            (void)write_response(response, use_brk2, request_id);
                        } catch (...) {
                            // Completion guard releases the per-session slot on every exit.
                        }
                    });
                if (!queued) {
                    completion.reset();
                    send_error(rejected_task_id, "broker worker queue is full", use_brk2, request_id);
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
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            workers_stopping_ = true;
        }
        queue_cv_.notify_all();
        for (auto& worker : workers_) {
            if (worker.joinable()) worker.join();
        }
        workers_.clear();
    }

    BrokerServerConfig config_;
    BrokerService service_;
    std::atomic<bool> running_{false};
    SocketHandle listener_socket_ = kInvalidSocket;
    std::size_t max_queued_tasks_ = 4096;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<std::function<void()>> task_queue_;
    bool workers_stopping_ = false;
    std::vector<std::thread> workers_;
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
