#pragma once

#include <semaphore.h>
#include <unistd.h>
#include <iostream>
#include <atomic>

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
    inline void set_output(bool flage) { enoutput_ = flage; }
    inline bool get_output() { return enoutput_; }
    inline void set_stream(bool flage) { enstream_ = flage; }
    inline bool get_stream() { return enstream_; }
    
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
             const std::string& work_id = "") {
        nlohmann::json out_body;
        out_body["request_id"] = request_id_;
        out_body["work_id"] = work_id.empty() ? work_id_ : work_id;
        out_body["created"] = time(NULL);
        out_body["object"] = object;
        out_body["data"] = data;
        if (error_msg.empty()) {
            out_body["error"]["code"] = 0;
            out_body["error"]["message"] = "";
        } else {
            out_body["error"] = error_msg;
        }

        std::string out = out_body.dump();
        out += "\n";

        send_raw_to_pub(out);
        if (enoutput_) {
            return send_raw_to_usr(out);
        }
        return 0;
    }
};
}  // namespace StackFlows