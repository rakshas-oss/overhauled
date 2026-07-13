#include "adi_server.h"

#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <future>
#include <iostream>
#include <limits>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

#include <cublas_v2.h>
#include <cuda_runtime.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace nvlink::adi {

namespace {

constexpr size_t kLengthPrefixBytes = sizeof(uint32_t);

uint32_t byteswap32(uint32_t value) noexcept {
    return ((value & 0x000000ffU) << 24U) |
           ((value & 0x0000ff00U) << 8U) |
           ((value & 0x00ff0000U) >> 8U) |
           ((value & 0xff000000U) >> 24U);
}

uint64_t byteswap64(uint64_t value) noexcept {
    return ((value & 0x00000000000000ffULL) << 56U) |
           ((value & 0x000000000000ff00ULL) << 40U) |
           ((value & 0x0000000000ff0000ULL) << 24U) |
           ((value & 0x00000000ff000000ULL) << 8U) |
           ((value & 0x000000ff00000000ULL) >> 8U) |
           ((value & 0x0000ff0000000000ULL) >> 24U) |
           ((value & 0x00ff000000000000ULL) >> 40U) |
           ((value & 0xff00000000000000ULL) >> 56U);
}

uint64_t double_to_u64(double value) noexcept {
    uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

double u64_to_double(uint64_t bits) noexcept {
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void throw_cuda(cudaError_t status, const char* operation) {
    if (status == cudaSuccess) {
        return;
    }
    throw NVLinkError(std::string(operation) + ": " + cudaGetErrorString(status));
}

void throw_cublas(cublasStatus_t status, const char* operation) {
    if (status == CUBLAS_STATUS_SUCCESS) {
        return;
    }
    throw NVLinkError(std::string(operation) + " failed with cuBLAS status " +
                      std::to_string(static_cast<int>(status)));
}

class DeviceBuffer {
public:
    explicit DeviceBuffer(size_t element_count) {
        if (element_count == 0) {
            return;
        }
        throw_cuda(
            cudaMalloc(reinterpret_cast<void**>(&ptr_), element_count * sizeof(double)),
            "cudaMalloc"
        );
    }

    ~DeviceBuffer() {
        if (ptr_ != nullptr) {
            cudaFree(ptr_);
        }
    }

    DeviceBuffer(const DeviceBuffer&) = delete;
    DeviceBuffer& operator=(const DeviceBuffer&) = delete;

    double* get() noexcept { return ptr_; }
    const double* get() const noexcept { return ptr_; }

private:
    double* ptr_ = nullptr;
};

class CublasHandle {
public:
    CublasHandle() {
        throw_cublas(cublasCreate(&handle_), "cublasCreate");
    }

    ~CublasHandle() {
        if (handle_ != nullptr) {
            cublasDestroy(handle_);
        }
    }

    CublasHandle(const CublasHandle&) = delete;
    CublasHandle& operator=(const CublasHandle&) = delete;

    cublasHandle_t get() const noexcept { return handle_; }

private:
    cublasHandle_t handle_ = nullptr;
};

template <typename T>
class BlockingQueue {
public:
    bool push(T value) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) {
            return false;
        }
        queue_.push(std::move(value));
        cv_.notify_one();
        return true;
    }

    bool pop(T& value) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [&] { return closed_ || !queue_.empty(); });
        if (queue_.empty()) {
            return false;
        }
        value = std::move(queue_.front());
        queue_.pop();
        return true;
    }

    void close() {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        cv_.notify_all();
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<T> queue_;
    bool closed_ = false;
};

bool read_exact(int fd, void* buffer, size_t bytes) {
    auto* data = static_cast<uint8_t*>(buffer);
    size_t offset = 0;
    while (offset < bytes) {
        ssize_t received = ::recv(fd, data + offset, bytes - offset, 0);
        if (received == 0) {
            return false;
        }
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        offset += static_cast<size_t>(received);
    }
    return true;
}

bool write_exact(int fd, const void* buffer, size_t bytes) {
    const auto* data = static_cast<const uint8_t*>(buffer);
    size_t offset = 0;
    while (offset < bytes) {
        int flags = 0;
#ifdef MSG_NOSIGNAL
        flags |= MSG_NOSIGNAL;
#endif
        ssize_t sent = ::send(fd, data + offset, bytes - offset, flags);
        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        offset += static_cast<size_t>(sent);
    }
    return true;
}

GpuPayload decode_payload(const uint8_t* bytes) {
    GpuPayload payload(::nvlink::adi::PAYLOAD_DOUBLES, 0.0);
    for (size_t i = 0; i < ::nvlink::adi::PAYLOAD_DOUBLES; ++i) {
        uint64_t bits = 0;
        std::memcpy(&bits, bytes + (i * sizeof(double)), sizeof(bits));
        payload[i] = u64_to_double(be64_to_host(bits));
    }
    return payload;
}

std::array<uint8_t, ::nvlink::adi::PAYLOAD_BYTES> encode_payload(const GpuPayload& payload) {
    if (payload.size() != ::nvlink::adi::PAYLOAD_DOUBLES) {
        throw NVLinkError("ADI payload must contain exactly 5 doubles.");
    }

    std::array<uint8_t, ::nvlink::adi::PAYLOAD_BYTES> bytes{};
    for (size_t i = 0; i < ::nvlink::adi::PAYLOAD_DOUBLES; ++i) {
        const uint64_t encoded = host_to_be64(double_to_u64(payload[i]));
        std::memcpy(bytes.data() + (i * sizeof(double)), &encoded, sizeof(encoded));
    }
    return bytes;
}

GpuPayload diff_payload(const GpuPayload& current, const GpuPayload& previous) {
    if (current.size() != previous.size()) {
        throw NVLinkError("Cannot compute ADI delta for mismatched payload sizes.");
    }

    GpuPayload delta(current.size(), 0.0);
    for (size_t i = 0; i < current.size(); ++i) {
        delta[i] = current[i] - previous[i];
    }
    return delta;
}

uint64_t now_ns() noexcept {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count()
    );
}

void close_fd(int& fd) noexcept {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

} // namespace

uint32_t host_to_be32(uint32_t n) noexcept {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return byteswap32(n);
#else
    return n;
#endif
}

uint64_t host_to_be64(uint64_t n) noexcept {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return byteswap64(n);
#else
    return n;
#endif
}

uint32_t be32_to_host(uint32_t n) noexcept {
    return host_to_be32(n);
}

uint64_t be64_to_host(uint64_t n) noexcept {
    return host_to_be64(n);
}

GpuPayload default_gpu_compute(const GpuPayload& input, int gpu_idx) {
    if (input.size() != ::nvlink::adi::PAYLOAD_DOUBLES) {
        throw NVLinkError("ADI payload must contain exactly 5 doubles.");
    }

    throw_cuda(cudaSetDevice(gpu_idx), "cudaSetDevice");

    GpuPayload output = input;
    DeviceBuffer device_buffer(output.size());
    CublasHandle handle;

    throw_cuda(
        cudaMemcpy(
            device_buffer.get(),
            output.data(),
            output.size() * sizeof(double),
            cudaMemcpyHostToDevice
        ),
        "cudaMemcpy host->device"
    );

    const double alpha = 2.0;
    throw_cublas(
        cublasDscal(
            handle.get(),
            static_cast<int>(output.size()),
            &alpha,
            device_buffer.get(),
            1
        ),
        "cublasDscal"
    );

    throw_cuda(
        cudaMemcpy(
            output.data(),
            device_buffer.get(),
            output.size() * sizeof(double),
            cudaMemcpyDeviceToHost
        ),
        "cudaMemcpy device->host"
    );

    return output;
}

class AdiServer::Impl {
public:
    Impl(const GpuTopology& topology, const AdiServerConfig& config)
        : topology_(topology),
          config_(config),
          placer_(topology_, config.backlog_threshold),
          compute_fn_(config.gpu_compute_fn ? config.gpu_compute_fn : default_gpu_compute),
          queues_(static_cast<size_t>(topology_.num_gpus())) {
        if (topology_.num_gpus() == 0) {
            throw NVLinkError("ADI server requires at least one GPU.");
        }
    }

    ~Impl() {
        stop();
    }

    void run() {
        bool expected = false;
        if (!running_.compare_exchange_strong(expected, true)) {
            throw NVLinkError("ADI server is already running.");
        }

        try {
            topology_.enable_peer_access();
            start_gpu_workers();
            setup_listener();
            accept_loop();
        } catch (...) {
            stop();
            join_sessions();
            close_queues();
            join_workers();
            running_.store(false, std::memory_order_release);
            throw;
        }

        join_sessions();
        close_queues();
        join_workers();
        close_fd(listen_fd_);
        running_.store(false, std::memory_order_release);
    }

    void stop() noexcept {
        if (!running_.exchange(false)) {
            return;
        }

        {
            std::lock_guard<std::mutex> lock(listener_mutex_);
            if (listen_fd_ >= 0) {
                ::shutdown(listen_fd_, SHUT_RDWR);
                close_fd(listen_fd_);
            }
        }

        std::lock_guard<std::mutex> lock(client_mutex_);
        for (int fd : client_fds_) {
            ::shutdown(fd, SHUT_RDWR);
        }
    }

    bool is_running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

    size_t active_clients() const noexcept {
        return active_clients_.load(std::memory_order_acquire);
    }

private:
    void start_gpu_workers() {
        gpu_workers_.reserve(queues_.size());
        for (size_t gpu = 0; gpu < queues_.size(); ++gpu) {
            gpu_workers_.emplace_back([this, gpu] { gpu_worker(static_cast<int>(gpu)); });
        }
    }

    void setup_listener() {
        if (config_.port <= 0 || config_.port > std::numeric_limits<uint16_t>::max()) {
            throw NVLinkError("ADI server port must be in the range 1-65535.");
        }

        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            throw std::system_error(errno, std::generic_category(), "socket");
        }

        int reuse = 1;
        if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
            close_fd(fd);
            throw std::system_error(errno, std::generic_category(), "setsockopt");
        }

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<uint16_t>(config_.port));
        address.sin_addr.s_addr = htonl(INADDR_ANY);

        if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
            close_fd(fd);
            throw std::system_error(errno, std::generic_category(), "bind");
        }

        if (::listen(fd, SOMAXCONN) < 0) {
            close_fd(fd);
            throw std::system_error(errno, std::generic_category(), "listen");
        }

        std::lock_guard<std::mutex> lock(listener_mutex_);
        listen_fd_ = fd;
    }

    void accept_loop() {
        while (running_.load(std::memory_order_acquire)) {
            sockaddr_in client_addr{};
            socklen_t client_len = sizeof(client_addr);
            int client_fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
            if (client_fd < 0) {
                if (!running_.load(std::memory_order_acquire)) {
                    break;
                }
                if (errno == EINTR) {
                    continue;
                }
                throw std::system_error(errno, std::generic_category(), "accept");
            }

            const int client_id = next_client_id_.fetch_add(1, std::memory_order_relaxed);
            {
                std::lock_guard<std::mutex> lock(client_mutex_);
                client_fds_.insert(client_fd);
            }
            active_clients_.fetch_add(1, std::memory_order_relaxed);

            session_threads_.emplace_back([this, client_fd, client_id] {
                handle_session(client_fd, client_id);
            });
        }
    }

    void handle_session(int client_fd, int client_id) {
        try {
            const int home_gpu = placer_.assign_home(client_id);
            if (config_.verbose) {
                std::cerr << "[ADI] client " << client_id << " assigned home GPU " << home_gpu << "\n";
            }

            GpuPayload last_sent_payload;
            bool have_previous = false;
            uint32_t sequence_id = 0;

            while (running_.load(std::memory_order_acquire)) {
                uint32_t length_be = 0;
                if (!read_exact(client_fd, &length_be, kLengthPrefixBytes)) {
                    break;
                }

                const uint32_t payload_length = be32_to_host(length_be);
                if (payload_length != ::nvlink::adi::EXPECTED_LENGTH) {
                    throw NVLinkError(
                        "Invalid ADI payload length: expected " +
                        std::to_string(::nvlink::adi::EXPECTED_LENGTH) +
                        ", got " + std::to_string(payload_length)
                    );
                }

                std::array<uint8_t, ::nvlink::adi::PAYLOAD_BYTES> request_bytes{};
                if (!read_exact(client_fd, request_bytes.data(), request_bytes.size())) {
                    break;
                }

                GpuTask task;
                task.input_data = decode_payload(request_bytes.data());
                task.target_gpu = placer_.place(client_id, [&](int gpu) {
                    return queues_[static_cast<size_t>(gpu)].size();
                });
                auto result_future = task.result_promise.get_future();

                if (!queues_[static_cast<size_t>(task.target_gpu)].push(std::move(task))) {
                    throw NVLinkError("GPU worker queue closed during ADI dispatch.");
                }

                GpuPayload primary_payload = result_future.get();
                send_packet(client_fd, sequence_id, PacketType::Primary, primary_payload);

                if (have_previous) {
                    send_packet(
                        client_fd,
                        sequence_id,
                        PacketType::Delta,
                        diff_payload(primary_payload, last_sent_payload)
                    );
                }

                last_sent_payload = std::move(primary_payload);
                have_previous = true;
                ++sequence_id;
            }
        } catch (const std::exception& ex) {
            if (config_.verbose) {
                std::cerr << "[ADI] client " << client_id << " error: " << ex.what() << "\n";
            }
        }

        placer_.release_client(client_id);
        {
            std::lock_guard<std::mutex> lock(client_mutex_);
            client_fds_.erase(client_fd);
        }
        ::shutdown(client_fd, SHUT_RDWR);
        ::close(client_fd);
        active_clients_.fetch_sub(1, std::memory_order_relaxed);
    }

    void send_packet(int client_fd, uint32_t sequence_id, PacketType packet_type, const GpuPayload& payload) {
        const ResponseHeader header{
            host_to_be64(now_ns()),
            host_to_be32(sequence_id),
            static_cast<uint8_t>(packet_type)
        };
        static_assert(sizeof(ResponseHeader) == 13, "ADI response header must be 13 bytes.");

        const auto payload_bytes = encode_payload(payload);
        if (!write_exact(client_fd, &header, sizeof(header)) ||
            !write_exact(client_fd, payload_bytes.data(), payload_bytes.size())) {
            throw NVLinkError("Failed to write ADI response packet.");
        }
    }

    void gpu_worker(int gpu_idx) noexcept {
        try {
            throw_cuda(cudaSetDevice(gpu_idx), "cudaSetDevice");

            GpuTask task;
            while (queues_[static_cast<size_t>(gpu_idx)].pop(task)) {
                try {
                    task.result_promise.set_value(compute_fn_(task.input_data, gpu_idx));
                } catch (...) {
                    task.result_promise.set_exception(std::current_exception());
                }
            }
        } catch (...) {
            const std::exception_ptr error = std::current_exception();
            queues_[static_cast<size_t>(gpu_idx)].close();

            GpuTask task;
            while (queues_[static_cast<size_t>(gpu_idx)].pop(task)) {
                task.result_promise.set_exception(error);
            }

            if (config_.verbose) {
                try {
                    std::rethrow_exception(error);
                } catch (const std::exception& ex) {
                    std::cerr << "[ADI] GPU worker " << gpu_idx << " failed: " << ex.what() << "\n";
                }
            }

            stop();
        }
    }

    void join_sessions() noexcept {
        for (auto& thread : session_threads_) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        session_threads_.clear();
    }

    void close_queues() noexcept {
        for (auto& queue : queues_) {
            queue.close();
        }
    }

    void join_workers() noexcept {
        for (auto& thread : gpu_workers_) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        gpu_workers_.clear();
    }

    const GpuTopology& topology_;
    AdiServerConfig config_;
    Placer placer_;
    GpuComputeFn compute_fn_;
    std::vector<BlockingQueue<GpuTask>> queues_;
    std::vector<std::thread> gpu_workers_;
    std::vector<std::thread> session_threads_;
    std::atomic<bool> running_{false};
    std::atomic<size_t> active_clients_{0};
    std::atomic<int> next_client_id_{1};
    mutable std::mutex listener_mutex_;
    mutable std::mutex client_mutex_;
    std::unordered_set<int> client_fds_;
    int listen_fd_ = -1;
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

size_t AdiServer::active_clients() const noexcept {
    return pimpl_->active_clients();
}

} // namespace nvlink::adi
