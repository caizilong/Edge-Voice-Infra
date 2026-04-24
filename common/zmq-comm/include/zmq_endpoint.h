#pragma once
#include <libzmq/zmq.h>
#include <unistd.h>
#include <atomic>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include "zmq_message.h"

#define ZMQ_RPC_FUN (ZMQ_REP | 0x80)
#define ZMQ_RPC_CALL (ZMQ_REQ | 0x80)

namespace StackFlows {
class ZmqEndpoint {
public:
    using rpc_callback_fun =
            std::function<std::string(ZmqEndpoint*, const std::shared_ptr<ZmqMessage>&)>;
    using msg_callback_fun = std::function<void(ZmqEndpoint*, const std::shared_ptr<ZmqMessage>&)>;

public:
    const int rpc_url_head_length = 6;
    std::string rpc_url_head_ = "ipc:///tmp/rpc.";
    void* zmq_ctx_;
    void* zmq_socket_;
    std::unordered_map<std::string, rpc_callback_fun> zmq_fun_;
    std::mutex zmq_fun_mtx_;
    std::atomic<bool> flage_;
    std::unique_ptr<std::thread> zmq_thread_;
    int mode_;
    std::string rpc_server_;
    std::string zmq_url_;
    int timeout_;

public:
    ZmqEndpoint(const std::string& url);
    ~ZmqEndpoint();

    void register_rpc_action(const std::string& action, const rpc_callback_fun& callback);
    void register_msg_action(const std::string& action, const msg_callback_fun& callback);
};
}  // namespace StackFlows