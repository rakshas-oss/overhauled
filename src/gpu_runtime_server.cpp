#include <iostream>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <future>
#include <chrono>
#include <atomic>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cuda_runtime.h>
#include <NvInfer.h>

#include "adi_tensor_protocol.h"
#include "trt_model_engine.hpp"

#define NUM_STREAMS_PER_GPU 16
#define MAGIC_ADI1 0x41444931

#define CUDA_CHECK(call) \
    do { \
        cudaError_t err = call; \
        if (err != cudaSuccess) { \
            fprintf(stderr, "CUDA Error at %s:%d - %s\n", __FILE__, __LINE__, cudaGetErrorString(err)); \
            exit(EXIT_FAILURE); \
        } \
    } while (0)

struct InferenceTask {
    uint32_t model_id;
    std::vector<uint8_t> input_payload;
    std::promise<std::vector<uint8_t>> promise;
};

template<typename T>
class TaskQueue {
public:
    void push(T value) {
        std::lock_guard<std::mutex> lock(m_);
        q_.push(std::move(value));
        cv_.notify_one();
    }

    bool try_pop(T& value) {
        std::lock_guard<std::mutex> lock(m_);
        if (q_.empty()) return false;
        value = std::move(q_.front());
        q_.pop();
        return true;
    }

private:
    std::queue<T> q_;
    std::mutex m_;
    std::condition_variable cv_;
};

struct StreamLane {
    cudaStream_t stream;
    std::unique_ptr<nvinfer1::IExecutionContext, void(*)(nvinfer1::IExecutionContext*)> context{nullptr, [](nvinfer1::IExecutionContext* c){ if(c) delete c; }};
    uint32_t active_model_id = 0;
    void* d_input = nullptr;
    void* d_output = nullptr;
    size_t input_cap = 0;
    size_t output_cap = 0;
    InferenceTask current_task;
    size_t output_size = 0;
    bool in_flight = false;
};

void gpu_worker_thread(int gpu_id, TaskQueue<InferenceTask>& queue) {
    CUDA_CHECK(cudaSetDevice(gpu_id));
    std::cout << "[GPU Worker " << gpu_id << "] Running with " << NUM_STREAMS_PER_GPU << " fractional lanes." << std::endl;

    std::vector<StreamLane> lanes(NUM_STREAMS_PER_GPU);
    for (int i = 0; i < NUM_STREAMS_PER_GPU; ++i) {
        CUDA_CHECK(cudaStreamCreate(&lanes[i].stream));
    }

    while (true) {
        bool busy = false;

        for (int i = 0; i < NUM_STREAMS_PER_GPU; ++i) {
            if (lanes[i].in_flight) {
                cudaError_t status = cudaStreamQuery(lanes[i].stream);
                if (status == cudaSuccess) {
                    std::vector<uint8_t> output_data(lanes[i].output_size);
                    CUDA_CHECK(cudaMemcpyAsync(output_data.data(), lanes[i].d_output, lanes[i].output_size, cudaMemcpyDeviceToHost, lanes[i].stream));
                    CUDA_CHECK(cudaStreamSynchronize(lanes[i].stream));

                    lanes[i].current_task.promise.set_value(std::move(output_data));
                    lanes[i].in_flight = false;
                    busy = true;
                }
            }
        }

        for (int i = 0; i < NUM_STREAMS_PER_GPU; ++i) {
            if (!lanes[i].in_flight) {
                InferenceTask task;
                if (queue.try_pop(task)) {
                    auto model = ModelManager::instance().getModel(task.model_id);
                    if (!model) {
                        task.promise.set_value(std::vector<uint8_t>());
                        continue;
                    }

                    if (lanes[i].active_model_id != task.model_id || !lanes[i].context) {
                        lanes[i].context.reset(model->engine->createExecutionContext());
                        lanes[i].active_model_id = task.model_id;
                    }

                    if (lanes[i].input_cap < model->total_input_bytes) {
                        if (lanes[i].d_input) cudaFree(lanes[i].d_input);
                        CUDA_CHECK(cudaMalloc(&lanes[i].d_input, model->total_input_bytes));
                        lanes[i].input_cap = model->total_input_bytes;
                    }
                    if (lanes[i].output_cap < model->total_output_bytes) {
                        if (lanes[i].d_output) cudaFree(lanes[i].d_output);
                        CUDA_CHECK(cudaMalloc(&lanes[i].d_output, model->total_output_bytes));
                        lanes[i].output_cap = model->total_output_bytes;
                    }

                    lanes[i].output_size = model->total_output_bytes;
                    lanes[i].current_task = std::move(task);

                    CUDA_CHECK(cudaMemcpyAsync(lanes[i].d_input, lanes[i].current_task.input_payload.data(), 
                                               model->total_input_bytes, cudaMemcpyHostToDevice, lanes[i].stream));

                    for (const auto& binding : model->input_bindings) {
                        lanes[i].context->setTensorAddress(binding.name.c_str(), lanes[i].d_input);
                    }
                    for (const auto& binding : model->output_bindings) {
                        lanes[i].context->setTensorAddress(binding.name.c_str(), lanes[i].d_output);
                    }

                    lanes[i].context->enqueueV3(lanes[i].stream);
                    lanes[i].in_flight = true;
                    busy = true;
                }
            }
        }

        if (!busy) {
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    }
}

void handle_client(int sock, std::vector<std::unique_ptr<TaskQueue<InferenceTask>>>& queues, std::atomic<uint32_t>& rr_counter) {
    uint32_t sequence_id = 0;
    std::vector<float> last_result_frame;

    try {
        while (true) {
            RequestHeader req_hdr;
            int n = read(sock, &req_hdr, sizeof(req_hdr));
            if (n <= 0) break;

            req_hdr.magic = ntohl(req_hdr.magic);
            req_hdr.payload_len = ntohl(req_hdr.payload_len);

            if (req_hdr.magic != MAGIC_ADI1) {
                std::cerr << "[Client Handler] Bad Magic Number." << std::endl;
                break;
            }

            std::vector<uint8_t> payload(req_hdr.payload_len);
            size_t bytes_read = 0;
            while (bytes_read < req_hdr.payload_len) {
                int r = read(sock, payload.data() + bytes_read, req_hdr.payload_len - bytes_read);
                if (r <= 0) throw std::runtime_error("Disconnected while reading payload.");
                bytes_read += r;
            }

            if (static_cast<PacketOpcode>(req_hdr.opcode) == PacketOpcode::LOAD_MODEL) {
                uint32_t model_id = *reinterpret_cast<uint32_t*>(payload.data());
                std::string onnx_path(reinterpret_cast<char*>(payload.data() + 4), req_hdr.payload_len - 4);
                bool ok = ModelManager::instance().loadOnnxModel(model_id, onnx_path);

                ResponseHeader resp{};
                resp.sequence_id = htonl(sequence_id++);
                resp.status_code = ok ? 0 : 1;
                write(sock, &resp, sizeof(resp));
            }
            else if (static_cast<PacketOpcode>(req_hdr.opcode) == PacketOpcode::EXECUTE_INFERENCE) {
                InferenceHeader* inf_hdr = reinterpret_cast<InferenceHeader*>(payload.data());
                
                std::vector<uint8_t> tensor_bytes(payload.begin() + sizeof(InferenceHeader), payload.end());
                InferenceTask task;
                task.model_id = inf_hdr->model_id;
                task.input_payload = std::move(tensor_bytes);
                auto future = task.promise.get_future();

                uint32_t q_idx = rr_counter.fetch_add(1) % queues.size();
                queues[q_idx]->push(std::move(task));

                std::vector<uint8_t> output_result = future.get();

                auto now = std::chrono::high_resolution_clock::now();
                uint64_t ts = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();

                ResponseHeader primary_hdr{};
                primary_hdr.timestamp_ns = htobe64(ts);
                primary_hdr.sequence_id = htonl(sequence_id);
                primary_hdr.packet_type = 0;
                primary_hdr.status_code = output_result.empty() ? 2 : 0;
                primary_hdr.payload_len = htonl(output_result.size());

                write(sock, &primary_hdr, sizeof(primary_hdr));
                write(sock, output_result.data(), output_result.size());

                size_t num_floats = output_result.size() / sizeof(float);
                const float* current_floats = reinterpret_cast<const float*>(output_result.data());

                if (!last_result_frame.empty() && last_result_frame.size() == num_floats) {
                    std::vector<float> delta(num_floats);
                    for (size_t i = 0; i < num_floats; ++i) {
                        delta[i] = current_floats[i] - last_result_frame[i];
                    }

                    ResponseHeader delta_hdr{};
                    delta_hdr.timestamp_ns = htobe64(ts);
                    delta_hdr.sequence_id = htonl(sequence_id);
                    delta_hdr.packet_type = 1;
                    delta_hdr.payload_len = htonl(delta.size() * sizeof(float));

                    write(sock, &delta_hdr, sizeof(delta_hdr));
                    write(sock, delta.data(), delta.size() * sizeof(float));
                }

                last_result_frame.assign(current_floats, current_floats + num_floats);
                sequence_id++;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[Client Handler] Exception: " << e.what() << std::endl;
    }
    close(sock);
}

int main(int argc, char* argv[]) {
    int port = argc > 1 ? std::stoi(argv[1]) : 8080;
    int num_gpus = 0;
    cudaGetDeviceCount(&num_gpus);

    if (num_gpus == 0) {
        std::cerr << "No CUDA devices detected." << std::endl;
        return 1;
    }

    std::vector<std::unique_ptr<TaskQueue<InferenceTask>>> queues;
    std::vector<std::thread> workers;

    for (int i = 0; i < num_gpus; ++i) {
        queues.push_back(std::make_unique<TaskQueue<InferenceTask>>());
        workers.emplace_back(gpu_worker_thread, i, std::ref(*queues[i]));
    }

    std::atomic<uint32_t> rr_counter(0);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    bind(server_fd, (struct sockaddr*)&addr, sizeof(addr));
    listen(server_fd, 64);

    std::cout << "[Server] Listening on port " << port << " across " << num_gpus << " GPUs." << std::endl;

    while (true) {
        sockaddr_in caddr;
        socklen_t clen = sizeof(caddr);
        int client_sock = accept(server_fd, (struct sockaddr*)&caddr, &clen);
        if (client_sock >= 0) {
            std::thread(handle_client, client_sock, std::ref(queues), std::ref(rr_counter)).detach();
        }
    }

    close(server_fd);
    return 0;
}
