#include <iostream>
#include "zmq_endpoint.h"

using namespace StackFlows;

int main() {
    ZmqEndpoint client("tcp://127.0.0.1:5555");
    
    std::cout << "[客户端] 准备发送 TCP 网络请求..." << std::endl;

    int ret = client.call_rpc_action("hello, ", "world！", [](ZmqEndpoint*, const std::shared_ptr<ZmqMessage>& resp) {
        std::cout << "[客户端] 收到服务端的回复: " << resp->string() << std::endl;
    });

    if (ret != 0) {
        std::cerr << "[客户端] 网络请求失败，错误码: " << ret << std::endl;
    }

    return 0;
}