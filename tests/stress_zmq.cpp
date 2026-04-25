#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <string>
#include "zmq_endpoint.h"

using namespace StackFlows;
using namespace std::chrono;

constexpr int NUM_CLIENT_THREADS = 10;
constexpr int REQUESTS_PER_THREAD = 1000;
const std::string RPC_SERVER_NAME = "stress_test_server";

std::atomic<int> success_count{0};
std::atomic<int> failure_count{0};

void start_server(std::jthread& server_thread) {
    server_thread = std::jthread([](std::stop_token st) {
        ZmqEndpoint server(RPC_SERVER_NAME);
        
        server.register_rpc_action("echo", [](ZmqEndpoint*, const std::shared_ptr<ZmqMessage>& msg) {
            return msg->string(); 
        });

        std::cout << "[Server] RPC Server started. Listening on ipc:///tmp/rpc." << RPC_SERVER_NAME << "\n";

        while (!st.stop_requested()) {
            std::this_thread::sleep_for(milliseconds(100));
        }
        std::cout << "[Server] RPC Server shutting down.\n";
    });
}

void client_worker(int client_id) {
    ZmqEndpoint client(RPC_SERVER_NAME);
    
    for (int i = 0; i < REQUESTS_PER_THREAD; ++i) {
        std::string req_data = "client_" + std::to_string(client_id) + "_req_" + std::to_string(i);
        bool call_success = false;

        int ret = client.call_rpc_action("echo", req_data, [&](ZmqEndpoint*, const std::shared_ptr<ZmqMessage>& resp) {
            if (resp->string() == req_data) {
                call_success = true;
            } else {
                std::cerr << "Data mismatch! Expected: " << req_data << ", Got: " << resp->string() << "\n";
            }
        });

        if (ret == 0 && call_success) {
            success_count.fetch_add(1, std::memory_order_relaxed);
        } else {
            failure_count.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

int main() {
    std::cout << "=== Starting ZeroMQ RPC Stress Test ===\n";
    std::cout << "Threads: " << NUM_CLIENT_THREADS << ", Requests per thread: " << REQUESTS_PER_THREAD << "\n";
    std::cout << "Total Requests: " << (NUM_CLIENT_THREADS * REQUESTS_PER_THREAD) << "\n\n";

    std::jthread server_thread;
    start_server(server_thread);
    std::this_thread::sleep_for(milliseconds(500));

    auto start_time = high_resolution_clock::now();
    
    std::vector<std::jthread> clients;
    for (int i = 0; i < NUM_CLIENT_THREADS; ++i) {
        clients.emplace_back(client_worker, i);
    }

    for (auto& client : clients) {
        client.join();
    }
    
    auto end_time = high_resolution_clock::now();
    auto duration = duration_cast<milliseconds>(end_time - start_time).count();

    server_thread.request_stop();
    server_thread.join();

    int total_success = success_count.load();
    int total_failure = failure_count.load();
    double qps = (duration > 0) ? (total_success * 1000.0 / duration) : 0;

    std::cout << "\n=== Test Results ===\n";
    std::cout << "Time elapsed : " << duration << " ms\n";
    std::cout << "Successful   : " << total_success << "\n";
    std::cout << "Failed       : " << total_failure << "\n";
    std::cout << "QPS          : " << qps << " req/sec\n";
    
    if (total_failure == 0 && total_success == (NUM_CLIENT_THREADS * REQUESTS_PER_THREAD)) {
        std::cout << "\n[PASS] Stress test completed perfectly without data loss or deadlocks.\n";
    } else {
        std::cout << "\n[FAIL] Stress test encountered errors.\n";
    }

    return 0;
}