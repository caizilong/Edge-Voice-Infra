#include "zmq_message.h"

namespace StackFlows {

ZmqMessage::ZmqMessage() { zmq_msg_init(&msg); }

ZmqMessage::~ZmqMessage() { zmq_msg_close(&msg); }

std::shared_ptr<std::string> ZmqMessage::get_string() {
    auto len = zmq_msg_size(&msg);
    return std::make_shared<std::string>((const char*)zmq_msg_data(&msg), len);
}

std::string ZmqMessage::string() {
    auto len = zmq_msg_size(&msg);
    return std::string((const char*)zmq_msg_data(&msg), len);
}

void* ZmqMessage::data() { return zmq_msg_data(&msg); }

size_t ZmqMessage::size() { return zmq_msg_size(&msg); }

zmq_msg_t* ZmqMessage::get() { return &msg; }

std::string ZmqMessage::get_param(int index, const std::string& idata) {
    const char* data = nullptr;
    int size = 0;

    if (idata.length() > 0) {
        data = idata.c_str();
        size = static_cast<int>(idata.length());
    } else {
        data = static_cast<const char*>(zmq_msg_data(&msg));
        size = static_cast<int>(zmq_msg_size(&msg));
    }

    if ((index % 2) == 0) {
        return std::string(data + 1, static_cast<size_t>(data[0]));
    } else {
        return std::string(data + data[0] + 1, static_cast<size_t>(size - data[0] - 1));
    }
}

std::string ZmqMessage::set_param(std::string param0, std::string param1) {
    std::string data = " " + param0 + param1;
    data[0] = static_cast<char>(param0.length());
    return data;
}

ZmqEndpoint::ZmqEndpoint(const std::string& server)
    : zmq_ctx_(NULL), zmq_socket_(NULL), rpc_server_(server), flage_(true), timeout_(3000) {
    if (server.find("://") != std::string::npos) {
        rpc_url_head_.clear();
    }
}

ZmqEndpoint::ZmqEndpoint(const std::string& url, int mode, const msg_callback_fun& raw_call)
    : zmq_ctx_(NULL), zmq_socket_(NULL), mode_(mode), flage_(true), timeout_(3000) {
    if ((url[0] != 'i') && (url[1] != 'p')) {
        rpc_url_head_.clear();
    }
    if (mode_ != ZMQ_RPC_FUN) {
        creat(url, raw_call);
    }
}

ZmqEndpoint::~ZmqEndpoint() {
    if (!zmq_socket_) {
        return;
    }
    flage_ = true;
    zmq_ctx_shutdown(zmq_ctx_);
    if (zmq_thread_ && zmq_thread_->joinable()) {
        zmq_thread_->join();
    }
    close_zmq();
}

bool ZmqEndpoint::is_bind() {
    return (mode_ == ZMQ_PUB) || (mode_ == ZMQ_PULL) || (mode_ == ZMQ_RPC_FUN);
}

void ZmqEndpoint::set_timeout(int ms) {
    timeout_ = ms;
}

int ZmqEndpoint::get_timeout() {
    return timeout_;
}

std::string ZmqEndpoint::_rpc_list_action(ZmqEndpoint* self, const std::shared_ptr<ZmqMessage>& _None) {
    std::string action_list;
    action_list.reserve(128);
    action_list = "{\"actions\":[";
    for (auto i = zmq_fun_.begin();;) {
        action_list += "\"";
        action_list += i->first;
        action_list += "\"";
        if (++i == zmq_fun_.end()) {
            action_list += "]}";
            break;
        } else {
            action_list += ",";
        }
    }
    return action_list;
}

int ZmqEndpoint::register_rpc_action(const std::string& action, const rpc_callback_fun& raw_call) {
    int ret = 0;
    std::unique_lock<std::mutex> lock(zmq_fun_mtx_);
    if (zmq_fun_.find(action) != zmq_fun_.end()) {
        zmq_fun_[action] = raw_call;
        return ret;
    }
    if (zmq_fun_.empty()) {
        std::string url = rpc_url_head_ + rpc_server_;
        mode_ = ZMQ_RPC_FUN;
        zmq_fun_["list_action"] =
            std::bind(&ZmqEndpoint::_rpc_list_action, this, std::placeholders::_1, std::placeholders::_2);
        ret = creat(url);
    }
    zmq_fun_[action] = raw_call;
    return ret;
}

void ZmqEndpoint::unregister_rpc_action(const std::string& action) {
    std::unique_lock<std::mutex> lock(zmq_fun_mtx_);
    if (zmq_fun_.find(action) != zmq_fun_.end()) {
        zmq_fun_.erase(action);
    }
}

int ZmqEndpoint::call_rpc_action(const std::string& action, const std::string& data, const msg_callback_fun& raw_call) {
    int ret = 0;
    std::shared_ptr<ZmqMessage> msg_ptr = std::make_shared<ZmqMessage>();
    try {
        if (NULL == zmq_socket_) {
            if (rpc_server_.empty()) return -1;
            std::string url = rpc_url_head_ + rpc_server_;
            mode_ = ZMQ_RPC_CALL;
            ret = creat(url);
            if (ret) {
                throw ret;
            }
        }
        // requist
        zmq_send(zmq_socket_, action.c_str(), action.length(), ZMQ_SNDMORE);
        zmq_send(zmq_socket_, data.c_str(), data.length(), 0);
        
        // action
        zmq_msg_recv(msg_ptr->get(), zmq_socket_, 0);
        
        if (raw_call) {
            raw_call(this, msg_ptr);
        }
    } catch (int e) {
        ret = e;
    }
    msg_ptr.reset();
    close_zmq();
    return ret;
}

int ZmqEndpoint::creat(const std::string& url, const msg_callback_fun& raw_call) {
    zmq_url_ = url;
    do {
        zmq_ctx_ = zmq_ctx_new();
    } while (zmq_ctx_ == NULL);
    do {
        zmq_socket_ = zmq_socket(zmq_ctx_, mode_ & 0x3f);
    } while (zmq_socket_ == NULL);

    switch (mode_) {
        case ZMQ_PUB:       return creat_pub(url);
        case ZMQ_SUB:       return subscriber_url(url, raw_call);
        case ZMQ_PUSH:      return creat_push(url);
        case ZMQ_PULL:      return creat_pull(url, raw_call);
        case ZMQ_RPC_FUN:   return creat_rep(url, raw_call);
        case ZMQ_RPC_CALL:  return creat_req(url);
        default:            break;
    }
    return 0;
}

int ZmqEndpoint::send_data(const std::string& raw) {
    return zmq_send(zmq_socket_, raw.c_str(), raw.length(), 0);
}

int ZmqEndpoint::creat_pub(const std::string& url) {
    return zmq_bind(zmq_socket_, url.c_str());
}

int ZmqEndpoint::creat_push(const std::string& url) {
    int reconnect_interval = 100;
    zmq_setsockopt(zmq_socket_, ZMQ_RECONNECT_IVL, &reconnect_interval, sizeof(reconnect_interval));
    int max_reconnect_interval = 1000;  // 5 seconds
    zmq_setsockopt(zmq_socket_, ZMQ_RECONNECT_IVL_MAX, &max_reconnect_interval, sizeof(max_reconnect_interval));
    zmq_setsockopt(zmq_socket_, ZMQ_SNDTIMEO, &timeout_, sizeof(timeout_));
    return zmq_connect(zmq_socket_, url.c_str());
}

int ZmqEndpoint::creat_pull(const std::string& url, const msg_callback_fun& raw_call) {
    int ret = zmq_bind(zmq_socket_, url.c_str());
    flage_ = false;
    zmq_thread_ = std::make_unique<std::thread>(std::bind(&ZmqEndpoint::zmq_event_loop, this, raw_call));
    return ret;
}

int ZmqEndpoint::subscriber_url(const std::string& url, const msg_callback_fun& raw_call) {
    int reconnect_interval = 100;
    zmq_setsockopt(zmq_socket_, ZMQ_RECONNECT_IVL, &reconnect_interval, sizeof(reconnect_interval));
    int max_reconnect_interval = 1000;  // 5 seconds
    zmq_setsockopt(zmq_socket_, ZMQ_RECONNECT_IVL_MAX, &max_reconnect_interval, sizeof(max_reconnect_interval));
    int ret = zmq_connect(zmq_socket_, url.c_str());
    zmq_setsockopt(zmq_socket_, ZMQ_SUBSCRIBE, "", 0);
    flage_ = false;
    zmq_thread_ = std::make_unique<std::thread>(std::bind(&ZmqEndpoint::zmq_event_loop, this, raw_call));
    return ret;
}

int ZmqEndpoint::creat_rep(const std::string& url, const msg_callback_fun& raw_call) {
    int ret = zmq_bind(zmq_socket_, url.c_str());
    flage_ = false;
    zmq_thread_ = std::make_unique<std::thread>(std::bind(&ZmqEndpoint::zmq_event_loop, this, raw_call));
    return ret;
}

int ZmqEndpoint::creat_req(const std::string& url) {
    if (!rpc_url_head_.empty()) {
        std::string socket_file = url.substr(rpc_url_head_length);
        if (access(socket_file.c_str(), F_OK) != 0) {
            return -1;
        }
    }
    zmq_setsockopt(zmq_socket_, ZMQ_SNDTIMEO, &timeout_, sizeof(timeout_));
    zmq_setsockopt(zmq_socket_, ZMQ_RCVTIMEO, &timeout_, sizeof(timeout_));
    return zmq_connect(zmq_socket_, url.c_str());
}

void ZmqEndpoint::zmq_event_loop(const msg_callback_fun& raw_call) {
    // Note: pthread_setname_np is platform dependent. Ensure your environment supports it.
#if defined(__linux__)
    pthread_setname_np(pthread_self(), "zmq_event_loop"); 
#endif

    int ret;
    zmq_pollitem_t items[1];
    if (mode_ == ZMQ_PULL) {
        items[0].socket = zmq_socket_;
        items[0].fd = 0;
        items[0].events = ZMQ_POLLIN;
        items[0].revents = 0;
    }
    
    while (!flage_.load()) {
        std::shared_ptr<ZmqMessage> msg_ptr = std::make_shared<ZmqMessage>();
        if (mode_ == ZMQ_PULL) {
            ret = zmq_poll(items, 1, -1);
            if (ret == -1) {
                zmq_close(zmq_socket_);
                continue;
            }
            if (!(items[0].revents & ZMQ_POLLIN)) {
                continue;
            }
        }
        ret = zmq_msg_recv(msg_ptr->get(), zmq_socket_, 0);
        if (ret <= 0) {
            msg_ptr.reset();
            continue;
        }

        if (mode_ == ZMQ_RPC_FUN) {
            std::shared_ptr<ZmqMessage> msg1_ptr = std::make_shared<ZmqMessage>();
            zmq_msg_recv(msg1_ptr->get(), zmq_socket_, 0);
            std::string retval;
            try {
                std::unique_lock<std::mutex> lock(zmq_fun_mtx_);
                retval = zmq_fun_.at(msg_ptr->string())(this, msg1_ptr);
            } catch (...) {
                retval = "NotAction";
            }
            zmq_send(zmq_socket_, retval.c_str(), retval.length(), 0);
            msg1_ptr.reset();
        } else {
            if (raw_call) {
                raw_call(this, msg_ptr);
            }
        }
        msg_ptr.reset();
    }
}

void ZmqEndpoint::close_zmq() {
    zmq_close(zmq_socket_);
    zmq_ctx_destroy(zmq_ctx_);
    if ((mode_ == ZMQ_PUB) || (mode_ == ZMQ_PULL) || (mode_ == ZMQ_RPC_FUN)) {
        if (!rpc_url_head_.empty()) {
            std::string socket_file = zmq_url_.substr(rpc_url_head_length);
            if (access(socket_file.c_str(), F_OK) == 0) {
                remove(socket_file.c_str());
            }
        }
    }
    zmq_socket_ = NULL;
    zmq_ctx_ = NULL;
}
}  // namespace StackFlows