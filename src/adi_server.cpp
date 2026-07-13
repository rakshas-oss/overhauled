#include "adi_server.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#if defined(NVLINK_PLACEMENT_HAS_CUDA) && NVLINK_PLACEMENT_HAS_CUDA
#include <cuda_runtime.h>
#endif

#if defined(NVLINK_ADI_HAS_CUBLAS) && NVLINK_ADI_HAS_CUBLAS
#include <cublas_v2.h>
#endif

namespace nvlink::adi {
namespace {

#if defined(_WIN32)
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
#endif

uint16_t host_to_be16(uint16_t value) noexcept {
#if defined(_WIN32)
    return htons(value);
#else
    return htons(value);
#endif
}

bool is_little_endian() noexcept {
    const uint16_t value = 0x1;
    return *reinterpret_cast<const uint8_t*>(&value) == 0x1;
}

uint64_t byteswap64(uint64_t n) noexcept {
    return ((n & 0x00000000000000ffULL) << 56) |
           ((n & 0x000000000000ff00ULL) << 40) |
           ((n & 0x0000000000ff0000ULL) << 24) |
           ((n & 0x00000000ff000000ULL) << 8) |
           ((n & 0x000000ff00000000ULL) >> 8) |
           ((n & 0x0000ff0000000000ULL) >> 24) |
           ((n & 0x00ff000000000000ULL) >> 40) |
           ((n & 0xff00000000000000ULL) >> 56);
}

uint64_t double_to_be64(double value) noexcept {
    uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return host_to_be64(bits);
}

double be64_to_double(uint64_t value) noexcept {
    uint64_t host = be64_to_host(value);
    double result = 0.0;
    std::memcpy(&result, &host, sizeof(result));
    return result;
}

bool read_exact(SocketHandle socket, void* buffer, std::size_t size) {
    uint8_t* out = static_cast<uint8_t*>(buffer);
    std::size_t total = 0;
    while (total < size) {
#if defined(_WIN32)
        int received = ::recv(socket, reinterpret_cast<char*>(out + total), static_cast<int>(size - total), 0);
#else
        ssize_t received = ::recv(socket, out + total, size - total, 0);
#endif
        if (received <= 0) {
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
#if defined(_WIN32)
        int written = ::send(socket, reinterpret_cast<const char*>(in + total), static_cast<int>(size - total), 0);
#else
        ssize_t written = ::send(socket, in + total, size - total, 0);
#endif
        if (written <= 0) {
            return false;
        }
        total += static_cast<std::size_t>(written);
    }
    return true;
}

void close_socket(SocketHandle socket) noexcept {
    if (socket == kInvalidSocket) {
        return;
    }
#if defined(_WIN32)
    ::closesocket(socket);
#else
    ::close(socket);
#endif
}

void shutdown_socket(SocketHandle socket) noexcept {
    if (socket == kInvalidSocket) {
        return;
    }
#if defined(_WIN32)
    ::shutdown(socket, SD_BOTH);
#else
    ::shutdown(socket, SHUT_RDWR);
#endif
}

uint64_t timestamp_now_ns() noexcept {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

void log_verbose(bool enabled, const std::string& message) {
    if (enabled) {
        std::cerr << "[ADI] " << message << '\n';
    }
}

#if defined(NVLINK_PLACEMENT_HAS_CUDA) && NVLINK_PLACEMENT_HAS_CUDA
void check_cuda(cudaError_t status, const char* step) {
    if (status != cudaSuccess) {
        throw NVLinkError(std::string(step) + ": " + cudaGetErrorString(status));
    }
}
#endif

#if defined(NVLINK_ADI_HAS_CUBLAS) && NVLINK_ADI_HAS_CUBLAS
void check_cublas(cublasStatus_t status, const char* step) {
    if (status != CUBLAS_STATUS_SUCCESS) {
        throw NVLinkError(std::string(step) + " failed");
    }
}
#endif

} // namespace

uint32_t host_to_be32(uint32_t n) noexcept {
    return htonl(n);
}

uint64_t host_to_be64(uint64_t n) noexcept {
    return is_little_endian() ? byteswap64(n) : n;
}

uint32_t be32_to_host(uint32_t n) noexcept {
    return ntohl(n);
}

uint64_t be64_to_host(uint64_t n) noexcept {
    return is_little_endian() ? byteswap64(n) : n;
}

GpuPayload default_gpu_compute(const GpuPayload& input, int gpu_idx) {
#if defined(NVLINK_PLACEMENT_HAS_CUDA) && NVLINK_PLACEMENT_HAS_CUDA && defined(NVLINK_ADI_HAS_CUBLAS) && NVLINK_ADI_HAS_CUBLAS
    if (input.empty()) {
        return input;
    }

    check_cuda(cudaSetDevice(gpu_idx), "cudaSetDevice");

    cublasHandle_t handle = nullptr;
    check_cublas(cublasCreate(&handle), "cublasCreate");

    double* device_buffer = nullptr;
    try {
        check_cuda(cudaMalloc(&device_buffer, input.size() * sizeof(double)), "cudaMalloc");
        check_cuda(cudaMemcpy(device_buffer,
                              input.data(),
                              input.size() * sizeof(double),
                              cudaMemcpyHostToDevice),
                   "cudaMemcpyHostToDevice");

        const double alpha = 2.0;
        check_cublas(cublasDscal(handle,
                                 static_cast<int>(input.size()),
                                 &alpha,
                                 device_buffer,
                                 1),
                     "cublasDscal");

        GpuPayload output(input.size());
        check_cuda(cudaMemcpy(output.data(),
                              device_buffer,
                              input.size() * sizeof(double),
                              cudaMemcpyDeviceToHost),
                   "cudaMemcpyDeviceToHost");
        cudaFree(device_buffer);
        cublasDestroy(handle);
        return output;
    } catch (...) {
        if (device_buffer != nullptr) {
            cudaFree(device_buffer);
        }
        if (handle != nullptr) {
            cublasDestroy(handle);
        }
        throw;
    }
#else
    (void)gpu_idx;
    GpuPayload output = input;
    for (double& value : output) {
        value *= 2.0;
    }
    return output;
#endif
}

class AdiServer::Impl {
public:
    Impl(const GpuTopology& topology, const AdiServerConfig& config)
        : topology_(topology),
          config_(config),
          placer_(topology_, config.backlog_threshold),
          compute_fn_(config.gpu_compute_fn ? std::move(config.gpu_compute_fn) : GpuComputeFn(default_gpu_compute)) {
        if (topology_.num_gpus() <= 0) {
            throw NVLinkError("ADI server requires at least one GPU in the provided topology.");
        }
        worker_states_.reserve(static_cast<std::size_t>(topology_.num_gpus()));
        for (int gpu = 0; gpu < topology_.num_gpus(); ++gpu) {
            worker_states_.push_back(std::make_unique<WorkerState>());
        }
    }

    ~Impl() {
        stop();
        join_all();
    }

    void run() {
#if defined(_WIN32)
        throw NVLinkError("ADI server networking is not implemented on Windows in this build.");
#else
        bool expected = false;
        if (!running_.compare_exchange_strong(expected, true)) {
            throw NVLinkError("ADI server is already running.");
        }

        try {
            start_workers();
            open_listener();
            accept_loop();
        } catch (...) {
            stop();
            join_all();
            throw;
        }

        join_all();
#endif
    }

    void stop() noexcept {
        if (!running_.exchange(false)) {
            return;
        }

        shutdown_socket(listener_socket_);
        close_socket(listener_socket_);
        listener_socket_ = kInvalidSocket;

        {
            std::lock_guard<std::mutex> lock(client_mutex_);
            for (SocketHandle socket : client_sockets_) {
                shutdown_socket(socket);
            }
        }

        for (auto& state : worker_states_) {
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->stopping = true;
            }
            state->cv.notify_all();
        }
    }

    bool is_running() const noexcept {
        return running_.load();
    }

    std::size_t active_clients() const noexcept {
        std::lock_guard<std::mutex> lock(client_mutex_);
        return client_sockets_.size();
    }

private:
    struct WorkerState {
        mutable std::mutex mutex;
        std::condition_variable cv;
        std::deque<std::shared_ptr<GpuTask>> queue;
        bool stopping = false;

        std::size_t size() const {
            std::lock_guard<std::mutex> lock(mutex);
            return queue.size();
        }
    };

    void start_workers() {
        if (!worker_threads_.empty()) {
            return;
        }
        for (int gpu = 0; gpu < topology_.num_gpus(); ++gpu) {
            worker_threads_.emplace_back([this, gpu] { gpu_worker_loop(gpu); });
        }
    }

    void open_listener() {
        listener_socket_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listener_socket_ == kInvalidSocket) {
            throw NVLinkError("Failed to create ADI server socket.");
        }

        const int reuse = 1;
        ::setsockopt(listener_socket_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = host_to_be16(static_cast<uint16_t>(config_.port));
        addr.sin_addr.s_addr = host_to_be32(INADDR_ANY);

        if (::bind(listener_socket_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            const int saved_errno = errno;
            close_socket(listener_socket_);
            listener_socket_ = kInvalidSocket;
            throw NVLinkError(std::string("Failed to bind ADI server socket: ") + std::strerror(saved_errno));
        }

        if (::listen(listener_socket_, SOMAXCONN) != 0) {
            const int saved_errno = errno;
            close_socket(listener_socket_);
            listener_socket_ = kInvalidSocket;
            throw NVLinkError(std::string("Failed to listen on ADI server socket: ") + std::strerror(saved_errno));
        }

        log_verbose(config_.verbose, "Listening on port " + std::to_string(config_.port));
    }

    void accept_loop() {
        while (running_.load()) {
            sockaddr_in client_addr{};
            socklen_t client_len = sizeof(client_addr);
            SocketHandle client_socket = ::accept(listener_socket_, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
            if (client_socket == kInvalidSocket) {
                if (!running_.load()) {
                    break;
                }
                if (errno == EINTR) {
                    continue;
                }
                throw NVLinkError(std::string("accept failed: ") + std::strerror(errno));
            }

            const int client_id = next_client_id_.fetch_add(1, std::memory_order_relaxed);
            {
                std::lock_guard<std::mutex> lock(client_mutex_);
                client_sockets_.insert(client_socket);
            }
            session_threads_.emplace_back([this, client_socket, client_id] { session_loop(client_socket, client_id); });
        }
    }

    void gpu_worker_loop(int gpu_idx) {
        WorkerState& state = *worker_states_[static_cast<std::size_t>(gpu_idx)];
        for (;;) {
            std::shared_ptr<GpuTask> task;
            {
                std::unique_lock<std::mutex> lock(state.mutex);
                state.cv.wait(lock, [&] { return state.stopping || !state.queue.empty(); });
                if (state.stopping && state.queue.empty()) {
                    return;
                }
                task = std::move(state.queue.front());
                state.queue.pop_front();
            }

            try {
                task->result_promise.set_value(compute_fn_(task->input_data, gpu_idx));
            } catch (...) {
                task->result_promise.set_exception(std::current_exception());
            }
        }
    }

    void session_loop(SocketHandle client_socket, int client_id) {
        try {
            int home_gpu = placer_.assign_home(client_id);
            log_verbose(config_.verbose,
                        "Accepted client " + std::to_string(client_id) +
                            " (home GPU " + std::to_string(home_gpu) + ")");

            GpuPayload previous_payload;
            bool first_exchange = true;
            uint32_t sequence_id = 0;

            while (running_.load()) {
                uint32_t length_be = 0;
                if (!read_exact(client_socket, &length_be, sizeof(length_be))) {
                    break;
                }
                uint32_t length = be32_to_host(length_be);
                if (length != EXPECTED_LENGTH) {
                    throw NVLinkError("Invalid ADI payload length: " + std::to_string(length));
                }

                std::array<uint64_t, PAYLOAD_DOUBLES> payload_words{};
                if (!read_exact(client_socket, payload_words.data(), PAYLOAD_BYTES)) {
                    break;
                }

                GpuPayload request(PAYLOAD_DOUBLES);
                for (std::size_t i = 0; i < PAYLOAD_DOUBLES; ++i) {
                    request[i] = be64_to_double(payload_words[i]);
                }

                int target_gpu = placer_.place(client_id, [this](int gpu) {
                    return worker_states_[static_cast<std::size_t>(gpu)]->size();
                });

                auto task = std::make_shared<GpuTask>();
                task->input_data = request;
                task->target_gpu = target_gpu;
                std::future<GpuPayload> result_future = task->result_promise.get_future();

                WorkerState& state = *worker_states_[static_cast<std::size_t>(target_gpu)];
                {
                    std::lock_guard<std::mutex> lock(state.mutex);
                    state.queue.push_back(task);
                }
                state.cv.notify_one();

                GpuPayload current = result_future.get();
                send_packet(client_socket, sequence_id, PacketType::Primary, current);
                if (!first_exchange) {
                    GpuPayload delta(current.size(), 0.0);
                    for (std::size_t i = 0; i < current.size(); ++i) {
                        delta[i] = current[i] - previous_payload[i];
                    }
                    send_packet(client_socket, sequence_id, PacketType::Delta, delta);
                }

                previous_payload = std::move(current);
                first_exchange = false;
                ++sequence_id;
            }
        } catch (const std::exception& ex) {
            log_verbose(config_.verbose,
                        "Closing client " + std::to_string(client_id) + ": " + ex.what());
        }

        placer_.release_client(client_id);
        {
            std::lock_guard<std::mutex> lock(client_mutex_);
            client_sockets_.erase(client_socket);
        }
        shutdown_socket(client_socket);
        close_socket(client_socket);
    }

    void send_packet(SocketHandle client_socket,
                     uint32_t sequence_id,
                     PacketType packet_type,
                     const GpuPayload& payload) {
        if (payload.size() != PAYLOAD_DOUBLES) {
            throw NVLinkError("ADI response payload must contain exactly 5 doubles.");
        }

        ResponseHeader header{};
        header.timestamp_ns = host_to_be64(timestamp_now_ns());
        header.sequence_id = host_to_be32(sequence_id);
        header.packet_type = static_cast<uint8_t>(packet_type);

        std::array<uint64_t, PAYLOAD_DOUBLES> payload_words{};
        for (std::size_t i = 0; i < PAYLOAD_DOUBLES; ++i) {
            payload_words[i] = double_to_be64(payload[i]);
        }

        if (!write_exact(client_socket, &header, sizeof(header)) ||
            !write_exact(client_socket, payload_words.data(), PAYLOAD_BYTES)) {
            throw NVLinkError("Failed to send ADI response packet.");
        }
    }

    void join_all() noexcept {
        shutdown_socket(listener_socket_);
        close_socket(listener_socket_);
        listener_socket_ = kInvalidSocket;

        for (auto& state : worker_states_) {
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->stopping = true;
            }
            state->cv.notify_all();
        }

        for (std::thread& thread : session_threads_) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        session_threads_.clear();

        for (std::thread& thread : worker_threads_) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        worker_threads_.clear();
    }

    const GpuTopology& topology_;
    AdiServerConfig config_;
    Placer placer_;
    GpuComputeFn compute_fn_;
    std::atomic<bool> running_{false};
    std::atomic<int> next_client_id_{1};
    SocketHandle listener_socket_ = kInvalidSocket;
    std::vector<std::unique_ptr<WorkerState>> worker_states_;
    std::vector<std::thread> worker_threads_;
    std::vector<std::thread> session_threads_;
    mutable std::mutex client_mutex_;
    std::unordered_set<SocketHandle> client_sockets_;
};

AdiServer::AdiServer(const GpuTopology& topology, const AdiServerConfig& config)
    : pimpl_(std::make_unique<Impl>(topology, config)) {}

AdiServer::~AdiServer() = default;

void AdiServer::run() {
    pimpl_->run();
}

void AdiServer::stop() noexcept {
    pimpl_->stop();
}

bool AdiServer::is_running() const noexcept {
    return pimpl_->is_running();
}

std::size_t AdiServer::active_clients() const noexcept {
    return pimpl_->active_clients();
}

} // namespace nvlink::adi
