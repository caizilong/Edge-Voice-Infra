#pragma once

#include <semaphore.h>
#include <unistd.h>
#include <iostream>
#include <atomic>

#include <condition_variable>
#include <deque>
#include <eventpp/eventqueue.h>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <regex>
#include <string>
#include <thread>
#include <unordered_map>
#include "StackFlowUtil.h"
#include "json.hpp"
#include "zmq_endpoint.h"

#define NODE_NO_ERROR std::string("")
#define NODE_NONE std::string("None")

namespace StackFlows {
class NodeChannel {
private:
    std::unordered_map<int, std::shared_ptr<ZmqEndpoint>> zmq_;
    std::atomic<int> zmq_url_index_;
    std::unordered_map<std::string, int> zmq_url_map_;
    std::mutex subscriber_mtx_;

    struct SendJob {
        std::string raw;
        bool to_pub = false;
        bool to_usr = false;
    };

    std::mutex send_mtx_;
    std::condition_variable send_cv_;
    std::deque<SendJob> send_queue_;
    std::jthread send_thread_;
    bool send_stopping_ = false;

    std::mutex state_mtx_;

    void send_loop(std::stop_token stoken);
    int enqueue_send(SendJob job);

public:
    std::string unit_name_;
    bool enoutput_;
    bool enstream_;
    std::string request_id_;
    std::string work_id_;
    std::string inference_url_;
    std::string publisher_url_;
    std::string output_url_;

    NodeChannel(const std::string& _publisher_url, const std::string& inference_url,
                const std::string& unit_name);
    ~NodeChannel();
    inline void set_output(bool flage) {
        std::scoped_lock lock(state_mtx_);
        enoutput_ = flage;
    }
    inline bool get_output() {
        std::scoped_lock lock(state_mtx_);
        return enoutput_;
    }
    inline void set_stream(bool flage) {
        std::scoped_lock lock(state_mtx_);
        enstream_ = flage;
    }
    inline bool get_stream() {
        std::scoped_lock lock(state_mtx_);
        return enstream_;
    }
    
    void subscriber_event_call(
            const std::function<void(const std::string&, const std::string&)>& call, ZmqEndpoint* _ZmqEndpoint,
            const std::shared_ptr<ZmqMessage>& raw);
    int subscriber_work_id(const std::string& work_id,
                           const std::function<void(const std::string&, const std::string&)>& call);
    void stop_subscriber_work_id(const std::string& work_id);
    void subscriber(const std::string& zmq_url, const ZmqEndpoint::msg_callback_fun& call);
    void stop_subscriber(const std::string& zmq_url);
    int send_raw_to_pub(const std::string& raw);
    int send_raw_to_usr(const std::string& raw);
    void set_push_url(const std::string& url);
    void clear_push_url();       
    static int send_raw_for_url(const std::string& zmq_url, const std::string& raw);

    int send(const std::string& object, const nlohmann::json& data, const std::string& error_msg,
             const std::string& work_id = "");
};
}  // namespace StackFlows
