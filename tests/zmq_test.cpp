#include <iostream>
#include <thread>
#include <chrono>
#include "zmq_endpoint.h"
#include "zmq_message.h"

using namespace StackFlows;

// 订阅者的回调函数：处理接收到的消息
void sub_callback(ZmqEndpoint* ep, const std::shared_ptr<ZmqMessage>& msg) {
    std::cout << "[SUB 接收] 收到消息: " << msg->string() << std::endl;
}

int main() {
    // 测试使用的本地 TCP 地址
    std::string url = "tcp://127.0.0.1:5555";

    std::cout << "--- ZMQ 测试开始 ---" << std::endl;

    // 1. 启动 Subscriber (接收端)
    std::cout << "1. 启动 Subscriber..." << std::endl;
    ZmqEndpoint sub(url, ZMQ_SUB, sub_callback);

    // 稍微等待一下，确保 SUB 连接完全建立（ZMQ的 PUB/SUB 存在慢连接问题，过早发消息会被丢弃）
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // 2. 启动 Publisher (发送端)
    std::cout << "2. 启动 Publisher..." << std::endl;
    ZmqEndpoint pub(url, ZMQ_PUB);

    // 3. 发送测试消息
    for (int i = 1; i <= 3; ++i) {
        std::string msg_data = "Hello ZMQ! 消息编号: " + std::to_string(i);
        std::cout << "[PUB 发送] 发送消息: " << msg_data << std::endl;
        pub.send_data(msg_data);
        
        // 间隔 500 毫秒发一条
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    // 4. 等待一段时间让 SUB 打印完接收到的消息
    std::this_thread::sleep_for(std::chrono::seconds(1));
    std::cout << "--- 测试结束 ---" << std::endl;

    return 0;
}