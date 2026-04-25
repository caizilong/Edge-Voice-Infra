#pragma once
#include <zmq.h>
#include <unistd.h>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <string_view>
#include "zmq_message.h"

#define ZMQ_RPC_FUN (ZMQ_REP | 0x80)
#define ZMQ_RPC_CALL (ZMQ_REQ | 0x80)

namespace StackFlows {

struct StringHash {
    using is_transparent = void;
    [[nodiscard]] size_t operator()(std::string_view txt) const {
        return std::hash<std::string_view>{}(txt);
    }
    [[nodiscard]] size_t operator()(const std::string& txt) const {
        return std::hash<std::string>{}(txt);
    }
};

class ZmqEndpoint {
public:
    using rpc_callback_fun =
            std::function<std::string(ZmqEndpoint*, const std::shared_ptr<ZmqMessage>&)>;
    using msg_callback_fun = std::function<void(ZmqEndpoint*, const std::shared_ptr<ZmqMessage>&)>;

public:
    static constexpr int rpc_url_head_length = 6;
    std::string rpc_url_head_ = "ipc:///tmp/rpc.";
    void* zmq_ctx_ = nullptr;
    void* zmq_socket_ = nullptr;
    
    std::unordered_map<std::string, rpc_callback_fun, StringHash, std::equal_to<>> zmq_fun_;
    std::mutex zmq_fun_mtx_;
    
    std::jthread zmq_thread_;
    
    int mode_;
    std::string rpc_server_;
    std::string zmq_url_;
    int timeout_;

public:
    explicit ZmqEndpoint(std::string_view server);
    ZmqEndpoint(std::string_view url, int mode, const msg_callback_fun& raw_call = nullptr);
    ~ZmqEndpoint();

    [[nodiscard]] bool is_bind() const;
    void set_timeout(int ms);
    [[nodiscard]] int get_timeout() const;

    int register_rpc_action(std::string_view action, const rpc_callback_fun& raw_call);
    void unregister_rpc_action(std::string_view action);
    int call_rpc_action(std::string_view action, std::string_view data, const msg_callback_fun& raw_call);
    
    int send_data(std::string_view raw);

private:
    std::string _rpc_list_action(ZmqEndpoint* self, const std::shared_ptr<ZmqMessage>& _None);
    int creat(std::string_view url, const msg_callback_fun& raw_call = nullptr);
    int creat_pub(std::string_view url);
    int creat_push(std::string_view url);
    int creat_pull(std::string_view url, const msg_callback_fun& raw_call);
    int subscriber_url(std::string_view url, const msg_callback_fun& raw_call);
    int creat_rep(std::string_view url, const msg_callback_fun& raw_call);
    int creat_req(std::string_view url);
    
    void zmq_event_loop(std::stop_token stoken, const msg_callback_fun& raw_call);
    void close_zmq();
};
}  // namespace StackFlows