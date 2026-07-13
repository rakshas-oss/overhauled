#include "nvlink_placement.h"
#include <iostream>
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>

using namespace nvlink;

struct ThreadSafeGpuQueue {
    std::queue<int> tasks;
    mutable std::mutex mutex;
    
    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex);
        return tasks.size();
    }
    
    void push(int task_id) {
        std::lock_guard<std::mutex> lock(mutex);
        tasks.push(task_id);
    }
};

int main() {
    try {
        std::cout << "=== NVLink Placement - Multi-Client Example ===\n\n";
        
        GpuTopology topo = GpuTopology::detect();
        topo.print_info();
        topo.enable_peer_access();
        
        Placer placer(topo, 32);
        std::vector<ThreadSafeGpuQueue> gpu_queues(topo.num_gpus());
        std::atomic<int> total_tasks(0);
        
        std::cout << "\n";
        
        auto client_worker = [&](int client_id) {
            int home_gpu = placer.assign_home(client_id);
            std::cout << "Client " << client_id << " starting (home GPU: " << home_gpu << ")\n";
            
            for (int task_num = 0; task_num < 10; ++task_num) {
                int target_gpu = placer.place(client_id, [&](int gpu) {
                    return gpu_queues[gpu].size();
                });
                
                gpu_queues[target_gpu].push(task_num);
                total_tasks.fetch_add(1, std::memory_order_relaxed);
                
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            
            placer.release_client(client_id);
            std::cout << "Client " << client_id << " done\n";
        };
        
        std::vector<std::thread> clients;
        for (int i = 0; i < 5; ++i) {
            clients.emplace_back(client_worker, i);
        }
        
        for (auto& t : clients) {
            t.join();
        }
        
        std::cout << "\n=== Results ===\n";
        std::cout << "Total tasks submitted: " << total_tasks << "\n";
        std::cout << "GPU Queue Depths:\n";
        
        size_t total_queued = 0;
        for (int i = 0; i < topo.num_gpus(); ++i) {
            size_t depth = gpu_queues[i].size();
            std::cout << "  GPU " << i << ": " << depth << " tasks\n";
            total_queued += depth;
        }
        std::cout << "Total queued: " << total_queued << "\n";
        
        if (topo.num_gpus() > 1) {
            std::cout << "\n=== Load Balance ===\n";
            std::vector<size_t> depths;
            for (int i = 0; i < topo.num_gpus(); ++i) {
                depths.push_back(gpu_queues[i].size());
            }
            
            auto min_depth = *std::min_element(depths.begin(), depths.end());
            auto max_depth = *std::max_element(depths.begin(), depths.end());
            double imbalance = (max_depth > 0) ? (100.0 * (max_depth - min_depth)) / max_depth : 0.0;
            
            std::cout << "Min depth: " << min_depth << "\n";
            std::cout << "Max depth: " << max_depth << "\n";
            std::cout << "Imbalance: " << imbalance << "%\n";
            std::cout << "(Lower is better; 0% = perfect balance)\n";
        }
        
        return 0;
        
    } catch (const NVLinkError& e) {
        std::cerr << "NVLink Error: " << e.what() << "\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}
