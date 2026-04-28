#include "zmq_endpoint.h"

namespace StackFlows {

ZmqEndpoint::ZmqEndpoint(std::string_view server)
    : rpc_server_(server), timeout_(3000) {
    if (server.find("://") != std::string_view::npos) {
        rpc_url_head_.clear();
    }
}

ZmqEndpoint::ZmqEndpoint(std::string_view url, int mode, const msg_callback_fun& raw_call)
    : mode_(mode), timeout_(3000) {
    // If caller passed a full URL (contains "://"), don't prepend the default rpc head
    if (url.find("://") != std::string_view::npos) {
        rpc_url_head_.clear();
    }
    // For non-rep (RPC function) sockets create immediately; RPC server sockets are
    // created lazily when the first rpc action is registered.
    if (mode_ != ZMQ_RPC_FUN) {
        creat(url, raw_call);
    }
}

ZmqEndpoint::~ZmqEndpoint() {
    if (!zmq_socket_) return;
    
    zmq_ctx_shutdown(zmq_ctx_);
    zmq_thread_.request_stop();

    if (zmq_thread_.joinable()) {
        zmq_thread_.join();
    }
    
    close_zmq();
}

bool ZmqEndpoint::is_bind() const {
    return (mode_ == ZMQ_PUB) || (mode_ == ZMQ_PULL) || (mode_ == ZMQ_RPC_FUN);
}

void ZmqEndpoint::set_timeout(int ms) { timeout_ = ms; }

int ZmqEndpoint::get_timeout() const { return timeout_; }

std::string ZmqEndpoint::_rpc_list_action(ZmqEndpoint* /*self*/, const std::shared_ptr<ZmqMessage>& /*_None*/) {
    std::scoped_lock lock(zmq_fun_mtx_);
    std::string action_list = "{\"actions\":[";
    for (auto it = zmq_fun_.begin(); it != zmq_fun_.end(); ++it) {
        action_list += "\"";
        action_list += it->first;
        action_list += "\"";
        if (std::next(it) != zmq_fun_.end()) {
            action_list += ",";
        }
    }
    action_list += "]}";
    return action_list;
}

int ZmqEndpoint::register_rpc_action(std::string_view action, const rpc_callback_fun& raw_call) {
    int ret = 0;
    std::scoped_lock lock(zmq_fun_mtx_);
    
    if (zmq_fun_.contains(action)) {
        zmq_fun_[std::string{action}] = raw_call;
        return ret;
    }
    
    if (zmq_fun_.empty()) {
        std::string url = rpc_url_head_ + rpc_server_;
        mode_ = ZMQ_RPC_FUN;
        zmq_fun_["list_action"] = [this](ZmqEndpoint* self, const std::shared_ptr<ZmqMessage>& msg) {
            return _rpc_list_action(self, msg);
        };
        ret = creat(url);
    }
    zmq_fun_.insert_or_assign(std::string{action}, raw_call);
    return ret;
}

void ZmqEndpoint::unregister_rpc_action(std::string_view action) {
    std::scoped_lock lock(zmq_fun_mtx_);
    auto it = zmq_fun_.find(action);
    if (it != zmq_fun_.end()) {
        zmq_fun_.erase(it);
    }
}

int ZmqEndpoint::call_rpc_action(std::string_view action, std::string_view data, const msg_callback_fun& raw_call) {
    int ret = 0;
    auto msg_ptr = std::make_shared<ZmqMessage>();
    try {
        if (zmq_socket_ == nullptr) {
            if (rpc_server_.empty()) return -1;
            std::string url = rpc_url_head_ + rpc_server_;
            mode_ = ZMQ_RPC_CALL;
            ret = creat(url);
            if (ret) throw ret;
        }
        
        if (zmq_send(zmq_socket_, action.data(), action.length(), ZMQ_SNDMORE) < 0) {
            throw -1;
        }
        if (zmq_send(zmq_socket_, data.data(), data.length(), 0) < 0) {
            throw -1;
        }
        if (zmq_msg_recv(msg_ptr->get(), zmq_socket_, 0) < 0) {
            throw -1;
        }
        if (raw_call) raw_call(this, msg_ptr);
        
    } catch (int e) {
        ret = e;
    }
    close_zmq();
    return ret;
}

int ZmqEndpoint::creat(std::string_view url, const msg_callback_fun& raw_call) {
    zmq_url_ = std::string{url};
    zmq_ctx_ = zmq_ctx_new();
    if (zmq_ctx_ == nullptr) return -1;
    zmq_socket_ = zmq_socket(zmq_ctx_, mode_ & 0x3f);
    if (zmq_socket_ == nullptr) {
        zmq_ctx_destroy(zmq_ctx_);
        zmq_ctx_ = nullptr;
        return -1;
    }

    switch (mode_) {
        case ZMQ_PUB:       return creat_pub(url);
        case ZMQ_SUB:       return subscriber_url(url, raw_call);
        case ZMQ_PUSH:      return creat_push(url);
        case ZMQ_PULL:      return creat_pull(url, raw_call);
        case ZMQ_RPC_FUN:   return creat_rep(url, raw_call);
        case ZMQ_RPC_CALL:  return creat_req(url);
        default:            return 0;
    }
}

int ZmqEndpoint::send_data(std::string_view raw) {
    return zmq_send(zmq_socket_, raw.data(), raw.length(), 0);
}

int ZmqEndpoint::creat_pub(std::string_view url) {
    return zmq_bind(zmq_socket_, std::string{url}.c_str());
}

int ZmqEndpoint::creat_push(std::string_view url) {
    int reconnect_ivl = 100, max_ivl = 1000;
    zmq_setsockopt(zmq_socket_, ZMQ_RECONNECT_IVL, &reconnect_ivl, sizeof(reconnect_ivl));
    zmq_setsockopt(zmq_socket_, ZMQ_RECONNECT_IVL_MAX, &max_ivl, sizeof(max_ivl));
    zmq_setsockopt(zmq_socket_, ZMQ_SNDTIMEO, &timeout_, sizeof(timeout_));
    return zmq_connect(zmq_socket_, std::string{url}.c_str());
}

int ZmqEndpoint::creat_pull(std::string_view url, const msg_callback_fun& raw_call) {
    int ret = zmq_bind(zmq_socket_, std::string{url}.c_str());
    zmq_thread_ = std::jthread([this, raw_call](std::stop_token st) { this->zmq_event_loop(st, raw_call); });
    return ret;
}

int ZmqEndpoint::subscriber_url(std::string_view url, const msg_callback_fun& raw_call) {
    int reconnect_ivl = 100, max_ivl = 1000;
    zmq_setsockopt(zmq_socket_, ZMQ_RECONNECT_IVL, &reconnect_ivl, sizeof(reconnect_ivl));
    zmq_setsockopt(zmq_socket_, ZMQ_RECONNECT_IVL_MAX, &max_ivl, sizeof(max_ivl));
    int ret = zmq_connect(zmq_socket_, std::string{url}.c_str());
    zmq_setsockopt(zmq_socket_, ZMQ_SUBSCRIBE, "", 0);
    
    zmq_thread_ = std::jthread([this, raw_call](std::stop_token st) { this->zmq_event_loop(st, raw_call); });
    return ret;
}

int ZmqEndpoint::creat_rep(std::string_view url, const msg_callback_fun& raw_call) {
    int ret = zmq_bind(zmq_socket_, std::string{url}.c_str());
    zmq_thread_ = std::jthread([this, raw_call](std::stop_token st) { this->zmq_event_loop(st, raw_call); });
    return ret;
}

int ZmqEndpoint::creat_req(std::string_view url) {
    std::string final_url = std::string{url};
    if (!rpc_url_head_.empty()) {
        if (final_url.size() <= static_cast<size_t>(rpc_url_head_length)) return -1;
        std::string socket_file = final_url.substr(rpc_url_head_length);
        if (access(socket_file.c_str(), F_OK) != 0) return -1;
    }
    zmq_setsockopt(zmq_socket_, ZMQ_SNDTIMEO, &timeout_, sizeof(timeout_));
    zmq_setsockopt(zmq_socket_, ZMQ_RCVTIMEO, &timeout_, sizeof(timeout_));
    return zmq_connect(zmq_socket_, final_url.c_str());
}

void ZmqEndpoint::zmq_event_loop(std::stop_token stoken, const msg_callback_fun& raw_call) {
#if defined(__linux__)
    pthread_setname_np(pthread_self(), "zmq_event_loop"); 
#endif

    zmq_pollitem_t items[1];
    if (mode_ == ZMQ_PULL) {
        items[0].socket = zmq_socket_;
        items[0].fd = 0;
        items[0].events = ZMQ_POLLIN;
        items[0].revents = 0;
    }
    
    while (!stoken.stop_requested()) {
        auto msg_ptr = std::make_shared<ZmqMessage>();
        
        if (mode_ == ZMQ_PULL) {
            if (zmq_poll(items, 1, 100) == -1) {
                if (stoken.stop_requested()) break;
                continue;
            }
            if (!(items[0].revents & ZMQ_POLLIN)) continue;
        }
        
        int ret = zmq_msg_recv(msg_ptr->get(), zmq_socket_, 0);
        if (ret <= 0) {
            if (stoken.stop_requested()) break; 
            continue;
        }

        if (mode_ == ZMQ_RPC_FUN) {
            auto msg1_ptr = std::make_shared<ZmqMessage>();
            if (zmq_msg_recv(msg1_ptr->get(), zmq_socket_, 0) < 0) {
                if (stoken.stop_requested()) break;
                continue;
            }
            std::string retval;
            try {
                rpc_callback_fun callback;
                std::string_view req_action = msg_ptr->view();
                {
                    std::scoped_lock lock(zmq_fun_mtx_);
                    auto it = zmq_fun_.find(req_action);
                    if (it != zmq_fun_.end()) {
                        callback = it->second;
                    }
                }
                if (callback) {
                    retval = callback(this, msg1_ptr);
                } else {
                    retval = "NotAction";
                }
            } catch (...) {
                retval = "NotAction";
            }
            zmq_send(zmq_socket_, retval.c_str(), retval.length(), 0);
        } else {
            if (raw_call) raw_call(this, msg_ptr);
        }
    }
}

void ZmqEndpoint::close_zmq() {
    if (zmq_socket_) zmq_close(zmq_socket_);
    if (zmq_ctx_) zmq_ctx_destroy(zmq_ctx_);
    
    if ((mode_ == ZMQ_PUB) || (mode_ == ZMQ_PULL) || (mode_ == ZMQ_RPC_FUN)) {
        if (!rpc_url_head_.empty() && !zmq_url_.empty()) {
            std::string socket_file = zmq_url_.substr(rpc_url_head_length);
            if (access(socket_file.c_str(), F_OK) == 0) {
                remove(socket_file.c_str());
            }
        }
    }
    zmq_socket_ = nullptr;
    zmq_ctx_ = nullptr;
}

}  // namespace StackFlows
