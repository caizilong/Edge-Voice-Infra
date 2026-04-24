// 删除原有hv头文件，添加：
#pragma once

#include <memory>

#include "network/TcpServer.h"
#include "network/EventLoop.h"

#include "zmq_bus.h"
#include "network/TcpConnection.h"

class TcpSession : public zmq_bus_com
{
public:
    explicit TcpSession(const network::TcpConnectionPtr &conn)
        : conn_(conn) {}

    void send_data(const std::string &data) override
    {
        if (auto conn = conn_.lock(); conn && conn->connected())
        {
            conn->send(data);
            return;
        }
        printf("zmq_bus_com::send_data skipped: tcp connection closed\n");
    }

    std::weak_ptr<network::TcpConnection> conn_;
};
