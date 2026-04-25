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
#include "NodeChannel.h"
#include "json.hpp"
#include "zmq_endpoint.h"
// Ensure NodeChannel is included or declared
namespace StackFlows {
class NodeChannel; 

class StackFlow {
public:
    typedef enum {
        EVENT_NONE = 0,
        EVENT_SETUP,
        EVENT_EXIT,
        EVENT_PAUSE,
        EVENT_TASKINFO,
    } LOCAL_EVENT;

    std::string unit_name_;
    std::string request_id_;
    std::string out_zmq_url_;

    std::atomic<bool> exit_flage_;
    std::atomic<int> status_;

    eventpp::EventQueue<int, void(const std::shared_ptr<void>&)> event_queue_;
    std::unique_ptr<std::thread> even_loop_thread_;

    std::unique_ptr<ZmqEndpoint> rpc_ctx_;

    // Renamed map for general task channels
    std::unordered_map<int, std::shared_ptr<NodeChannel>> task_channels_;

    StackFlow(const std::string& unit_name);
    void even_loop();
    void _none_event(const std::shared_ptr<void>& arg);

    template <typename T> std::shared_ptr<NodeChannel> get_channel(T workid) {
        int _work_id_num;
        if constexpr (std::is_same<T, int>::value) {
            _work_id_num = workid;
        } else if constexpr (std::is_same<T, std::string>::value) {
            _work_id_num = sample_get_work_id_num(workid);
        } else {
            return nullptr;
        }
        return task_channels_.at(_work_id_num);
    }

    std::string _rpc_setup(ZmqEndpoint* _ZmqEndpoint, const std::shared_ptr<ZmqMessage>& data);
    void _setup(const std::shared_ptr<void>& arg) {
        std::shared_ptr<ZmqMessage> originalPtr = std::static_pointer_cast<ZmqMessage>(arg);
        std::string zmq_url = originalPtr->get_param(0);
        std::string data = originalPtr->get_param(1);

        request_id_ = sample_json_str_get(data, "request_id");
        out_zmq_url_ = zmq_url;
        if (status_.load()) setup(zmq_url, data);
    };
    virtual int setup(const std::string& zmq_url, const std::string& raw);
    virtual int setup(const std::string& work_id, const std::string& object,
                      const std::string& data);

    std::string _rpc_exit(ZmqEndpoint* _ZmqEndpoint, const std::shared_ptr<ZmqMessage>& data);
    void _exit(const std::shared_ptr<void>& arg) {
        std::shared_ptr<ZmqMessage> originalPtr = std::static_pointer_cast<ZmqMessage>(arg);
        std::string zmq_url = originalPtr->get_param(0);
        std::string data = originalPtr->get_param(1);
        request_id_ = sample_json_str_get(data, "request_id");
        out_zmq_url_ = zmq_url;
        if (status_.load()) exit(zmq_url, data);
    }
    virtual int exit(const std::string& zmq_url, const std::string& raw);
    virtual int exit(const std::string& work_id, const std::string& object,
                     const std::string& data);

    std::string _rpc_pause(ZmqEndpoint* _ZmqEndpoint, const std::shared_ptr<ZmqMessage>& data);
    void _pause(const std::shared_ptr<void>& arg) {
        std::shared_ptr<ZmqMessage> originalPtr = std::static_pointer_cast<ZmqMessage>(arg);
        std::string zmq_url = originalPtr->get_param(0);
        std::string data = originalPtr->get_param(1);
        request_id_ = sample_json_str_get(data, "request_id");
        out_zmq_url_ = zmq_url;
        if (status_.load()) pause(zmq_url, data);
    }
    virtual void pause(const std::string& zmq_url, const std::string& raw);
    virtual void pause(const std::string& work_id, const std::string& object,
                       const std::string& data);

    std::string _rpc_taskinfo(ZmqEndpoint* _ZmqEndpoint, const std::shared_ptr<ZmqMessage>& data);
    void _taskinfo(const std::shared_ptr<void>& arg) {
        std::shared_ptr<ZmqMessage> originalPtr = std::static_pointer_cast<ZmqMessage>(arg);
        std::string zmq_url = originalPtr->get_param(0);
        std::string data = originalPtr->get_param(1);
        request_id_ = sample_json_str_get(data, "request_id");
        out_zmq_url_ = zmq_url;
        if (status_.load()) taskinfo(zmq_url, data);
    }
    virtual void taskinfo(const std::string& zmq_url, const std::string& raw);
    virtual void taskinfo(const std::string& work_id, const std::string& object,
                          const std::string& data);

    int send(const std::string& object, const nlohmann::json& data, const std::string& error_msg,
             const std::string& work_id, const std::string& zmq_url = "") {
        nlohmann::json out_body;
        out_body["request_id"] = request_id_;
        out_body["work_id"] = work_id;
        out_body["created"] = time(NULL);
        out_body["object"] = object;
        out_body["data"] = data;
        if (error_msg.empty()) {
            out_body["error"]["code"] = 0;
            out_body["error"]["message"] = "";
        } else {
            out_body["error"] = error_msg;
        }

        if (zmq_url.empty()) {
            ZmqEndpoint _zmq(out_zmq_url_, ZMQ_PUSH);
            std::string out = out_body.dump();
            out += "\n";
            return _zmq.send_data(out);
        } else {
            ZmqEndpoint _zmq(zmq_url, ZMQ_PUSH);
            std::string out = out_body.dump();
            out += "\n";
            return _zmq.send_data(out);
        }
    }

    std::string sys_sql_select(const std::string& key);
    void sys_sql_set(const std::string& key, const std::string& val);
    void sys_sql_unset(const std::string& key);
    int sys_register_unit(const std::string& unit_name);
    template <typename T> bool sys_release_unit(T workid) {
        std::string _work_id;
        int _work_id_num;
        if constexpr (std::is_same<T, int>::value) {
            _work_id = sample_get_work_id(workid, unit_name_);
            _work_id_num = workid;
        } else if constexpr (std::is_same<T, std::string>::value) {
            _work_id = workid;
            _work_id_num = sample_get_work_id_num(workid);
        } else {
            return false;
        }
        ZmqEndpoint _call("sys");
        _call.call_rpc_action("release_unit", _work_id,
                              [](ZmqEndpoint* _ZmqEndpoint, const std::shared_ptr<ZmqMessage>& data) {});
        task_channels_[_work_id_num].reset();
        task_channels_.erase(_work_id_num);
        return false;
    }
    bool sys_release_unit(int work_id_num, const std::string& work_id);
    ~StackFlow();
};
};  // namespace StackFlows