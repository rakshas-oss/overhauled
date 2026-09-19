#include "broker_server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
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
        if (config_.max_inflight_per_client == 0) {
            throw std::invalid_argument("max_inflight_per_client must be greater than zero");
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
        if (!running_.exchange(false)) {
            return;
        }

        shutdown_socket(listener_socket_);
        close_socket(listener_socket_);
        listener_socket_ = kInvalidSocket;

        std::lock_guard<std::mutex> lock(client_mutex_);
        for (SocketHandle socket : client_sockets_) {
            shutdown_socket(socket);
        }
    }

    bool is_running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

private:
    struct SessionState {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };

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
        struct PendingResponse {
            std::thread thread;
            std::shared_ptr<std::atomic<bool>> done;
        };

        auto reap_finished = [](std::deque<PendingResponse>* pending, bool join_all) {
            auto it = pending->begin();
            while (it != pending->end()) {
                const bool finished = it->done != nullptr && it->done->load(std::memory_order_acquire);
                if (join_all || finished) {
                    if (it->thread.joinable()) {
                        it->thread.join();
                    }
                    it = pending->erase(it);
                } else {
                    ++it;
                }
            }
        };

        std::deque<PendingResponse> pending_responses;
        std::mutex write_mutex;
        auto writes_enabled = std::make_shared<std::atomic<bool>>(true);

        auto write_response = [&](const BrokerResponse& response) {
            if (!writes_enabled->load(std::memory_order_acquire)) {
                return false;
            }
            const std::vector<uint8_t> response_frame = encode_response_frame(
                response,
                config_.service_config.protocol_limits);
            const uint32_t response_len_be = htonl(static_cast<uint32_t>(response_frame.size()));
            std::lock_guard<std::mutex> write_lock(write_mutex);
            if (!writes_enabled->load(std::memory_order_acquire)) {
                return false;
            }
            return write_exact(client_socket, &response_len_be, sizeof(response_len_be)) &&
                   write_exact(client_socket, response_frame.data(), response_frame.size());
        };

        auto send_protocol_error_locked = [&](const std::string& task_id, const std::string& error) {
            if (!writes_enabled->load(std::memory_order_acquire)) {
                return;
            }
            std::lock_guard<std::mutex> write_lock(write_mutex);
            if (!writes_enabled->load(std::memory_order_acquire)) {
                return;
            }
            send_protocol_error(client_socket, task_id, error, config_.service_config.protocol_limits);
        };

        try {
            while (running_.load(std::memory_order_acquire)) {
                reap_finished(&pending_responses, false);
                uint32_t frame_len_be = 0;
                if (!read_exact(client_socket, &frame_len_be, sizeof(frame_len_be))) {
                    break;
                }

                const std::size_t frame_len = ntohl(frame_len_be);
                if (frame_len == 0 || frame_len > config_.service_config.protocol_limits.max_frame_bytes) {
                    send_protocol_error_locked("", "frame length violates protocol limits");
                    break;
                }

                std::vector<uint8_t> frame(frame_len);
                if (!read_exact(client_socket, frame.data(), frame.size())) {
                    break;
                }

                BrokerRequest request;
                std::string decode_error;
                if (!decode_request_frame(frame,
                                          &request,
                                          &decode_error,
                                          config_.service_config.protocol_limits)) {
                    send_protocol_error_locked(request.task_id, decode_error);
                    continue;
                }

                if (pending_responses.size() >= config_.max_inflight_per_client) {
                    send_protocol_error_locked(request.task_id, "too many inflight requests for client");
                    continue;
                }

                auto done = std::make_shared<std::atomic<bool>>(false);
                pending_responses.push_back(PendingResponse{
                    std::thread([this, request, write_response, done] {
                        try {
                            BrokerResponse response = service_.handle_request(request);
                            (void)write_response(response);
                        } catch (const std::exception&) {
                        }
                        done->store(true, std::memory_order_release);
                    }),
                    done,
                });
            }
        } catch (const std::exception&) {
        }

        writes_enabled->store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(client_mutex_);
            client_sockets_.erase(client_socket);
        }
        shutdown_socket(client_socket);
        reap_finished(&pending_responses, true);
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
    }

    BrokerServerConfig config_;
    BrokerService service_;
    std::atomic<bool> running_{false};
    SocketHandle listener_socket_ = kInvalidSocket;
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
