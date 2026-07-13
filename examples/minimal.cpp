#include "nvlink_placement.h"
#include <iostream>
#include <vector>
#include <queue>

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
        std::cout << "=== NVLink Placement Library - Minimal Example ===\n\n";
        
        std::cout << "Detecting GPU topology...\n";
        GpuTopology topo = GpuTopology::detect();
        topo.print_info();
        topo.enable_peer_access();
        std::cout << "\n";
        
        std::cout << "Creating placer with backlog threshold=32...\n";
        Placer placer(topo, 32);
        std::cout << "Placer created.\n\n";
        
        std::vector<ThreadSafeGpuQueue> gpu_queues(topo.num_gpus());
        
        for (int client_id = 0; client_id < 3; ++client_id) {
            int home_gpu = placer.assign_home(client_id);
            std::cout << "Client " << client_id << " assigned home GPU " << home_gpu << "\n";
            
            for (int task_num = 0; task_num < 5; ++task_num) {
                int target_gpu = placer.place(client_id, [&](int gpu) {
                    return gpu_queues[gpu].size();
                });
                
                gpu_queues[target_gpu].push(task_num);
                
                std::cout << "  Task " << task_num << " -> GPU " << target_gpu
                         << " (queue depth: " << gpu_queues[target_gpu].size() << ")\n";
            }
            
            placer.release_client(client_id);
            std::cout << "\n";
        }
        
        std::cout << "=== Final Queue Depths ===\n";
        for (int i = 0; i < topo.num_gpus(); ++i) {
            std::cout << "GPU " << i << ": " << gpu_queues[i].size() << " tasks\n";
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
