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
    ZmqEndpoint(const std::string& server);
    ZmqEndpoint(const std::string& url, int mode, const msg_callback_fun& raw_call = nullptr);
    ~ZmqEndpoint();

    bool is_bind();
    void set_timeout(int ms);
    int get_timeout();

    int register_rpc_action(const std::string& action, const rpc_callback_fun& raw_call);
    void unregister_rpc_action(const std::string& action);
    int call_rpc_action(const std::string& action, const std::string& data, const msg_callback_fun& raw_call);
    
    int send_data(const std::string& raw);

private:
    std::string _rpc_list_action(ZmqEndpoint* self, const std::shared_ptr<ZmqMessage>& _None);
    int creat(const std::string& url, const msg_callback_fun& raw_call = nullptr);
    int creat_pub(const std::string& url);
    int creat_push(const std::string& url);
    int creat_pull(const std::string& url, const msg_callback_fun& raw_call);
    int subscriber_url(const std::string& url, const msg_callback_fun& raw_call);
    int creat_rep(const std::string& url, const msg_callback_fun& raw_call);
    int creat_req(const std::string& url);
    void zmq_event_loop(const msg_callback_fun& raw_call);
    void close_zmq();
};
}  // namespace StackFlows