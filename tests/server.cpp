#include <iostream>
#include <thread>
#include "zmq_endpoint.h"

using namespace StackFlows;

int main() {
    ZmqEndpoint server("tcp://127.0.0.1:5555");
    
    server.register_rpc_action("hello", [](ZmqEndpoint*, const std::shared_ptr<ZmqMessage>& msg) {
        std::cout << "[服务端] 收到客户端跨网络发来的数据: " << msg->string() << std::endl;
        return "Hello! 我已收到你的消息: " + msg->string();
    });

    std::cout << "[服务端] 启动成功，正在 tcp://127.0.0.1:5555 监听..." << std::endl;
    
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    return 0;
}