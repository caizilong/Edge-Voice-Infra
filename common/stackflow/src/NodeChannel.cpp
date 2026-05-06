#include "NodeChannel.h"
#include <iostream>
#include "logger.h"

using namespace StackFlows;

NodeChannel::NodeChannel(const std::string& _publisher_url,
                         const std::string& inference_url, const std::string& unit_name)
        : unit_name_(unit_name),
          enoutput_(false),
          enstream_(false),
          inference_url_(inference_url),
          publisher_url_(_publisher_url) {
    zmq_url_index_ = -1000;
    send_thread_ = std::jthread([this](std::stop_token st) { send_loop(st); });
}

NodeChannel::~NodeChannel() { 
    stop_subscriber("");
    {
        std::scoped_lock lock(send_mtx_);
        send_stopping_ = true;
    }
    send_cv_.notify_all();
    if (send_thread_.joinable()) {
        send_thread_.request_stop();
        send_thread_.join();
    }
    std::cout << "NodeChannel 析构" << std::endl; 
}

void NodeChannel::send_loop(std::stop_token stoken) {
    ZmqEndpoint publisher(publisher_url_, ZMQ_PUB);
    std::unique_ptr<ZmqEndpoint> output;
    std::string active_output_url;

    while (true) {
        SendJob job;
        {
            std::unique_lock lock(send_mtx_);
            send_cv_.wait(lock, [&] {
                return send_stopping_ || stoken.stop_requested() || !send_queue_.empty();
            });
            if (send_queue_.empty()) {
                if (send_stopping_ || stoken.stop_requested()) {
                    break;
                }
                continue;
            }
            job = std::move(send_queue_.front());
            send_queue_.pop_front();
        }

        if (job.to_pub) {
            publisher.send_data(job.raw);
        }
        if (job.to_usr) {
            std::string target_url;
            {
                std::scoped_lock lock(state_mtx_);
                target_url = output_url_;
            }
            if (target_url.empty()) {
                continue;
            }
            if (!output || active_output_url != target_url) {
                output = std::make_unique<ZmqEndpoint>(target_url, ZMQ_PUSH);
                active_output_url = target_url;
            }
            output->send_data(job.raw);
        }
    }
}

int NodeChannel::enqueue_send(SendJob job) {
    {
        std::scoped_lock lock(send_mtx_);
        if (send_stopping_) {
            return -1;
        }
        send_queue_.push_back(std::move(job));
    }
    send_cv_.notify_one();
    return 0;
}

void NodeChannel::subscriber_event_call(
        const std::function<void(const std::string&, const std::string&)>& call, ZmqEndpoint* _ZmqEndpoint,
        const std::shared_ptr<ZmqMessage>& raw) {
    auto _raw = raw->string();
    const char* user_inference_flage_str = "\"action\"";
    std::size_t pos = _raw.find(user_inference_flage_str);
    while (true) {
        if (pos == std::string::npos) {
            break;
        } else if ((pos > 0) && (_raw[pos - 1] != '\\')) {
            std::string zmq_com = sample_json_str_get(_raw, "zmq_com");
            if (!zmq_com.empty()) set_push_url(zmq_com);
            {
                std::scoped_lock lock(state_mtx_);
                request_id_ = sample_json_str_get(_raw, "request_id");
                work_id_ = sample_json_str_get(_raw, "work_id");
            }
            break;
        }
        pos = _raw.find(user_inference_flage_str, pos + sizeof(user_inference_flage_str));
    }
    call(sample_json_str_get(_raw, "object"), sample_json_str_get(_raw, "data"));
}

void message_handler(ZmqEndpoint* zmq_obj, const std::shared_ptr<ZmqMessage>& data) {
    std::cout << "Received: " << data->string() << std::endl;
}

int NodeChannel::subscriber_work_id(
        const std::string& work_id,
        const std::function<void(const std::string&, const std::string&)>& call) {
    int id_num;
    std::string subscriber_url;
    std::regex pattern(R"((\w+)\.(\d+))");
    std::smatch matches;
    if ((!work_id.empty()) && std::regex_match(work_id, matches, pattern)) {
        if (matches.size() == 3) {
            id_num = std::stoi(matches[2].str());
            std::string input_url_name = work_id + ".out_port";
            std::string input_url = unit_call("sys", "sql_select", input_url_name);
            if (input_url.empty()) {
                return -1;
            }
            subscriber_url = input_url;
        }
    } else {
        id_num = 0;
        subscriber_url = inference_url_;
    }

    std::scoped_lock lock(subscriber_mtx_);
    zmq_[id_num] = std::make_shared<ZmqEndpoint>(
            subscriber_url, ZMQ_SUB,
            std::bind(&NodeChannel::subscriber_event_call, this, call,
                      std::placeholders::_1, std::placeholders::_2));
    return 0;
}

void NodeChannel::stop_subscriber_work_id(const std::string& work_id) {
    int id_num;
    std::regex pattern(R"((\w+)\.(\d+))");
    std::smatch matches;
    if (std::regex_match(work_id, matches, pattern)) {
        if (matches.size() == 3) {
            id_num = std::stoi(matches[2].str());
        }
    } else {
        id_num = 0;
    }
    std::scoped_lock lock(subscriber_mtx_);
    if (zmq_.find(id_num) != zmq_.end()) zmq_.erase(id_num);
}

void NodeChannel::subscriber(const std::string& zmq_url, const ZmqEndpoint::msg_callback_fun& call) {
    std::scoped_lock lock(subscriber_mtx_);
    zmq_url_map_[zmq_url] = zmq_url_index_--;
    zmq_[zmq_url_map_[zmq_url]] = std::make_shared<ZmqEndpoint>(zmq_url, ZMQ_SUB, call);
}

void NodeChannel::stop_subscriber(const std::string& zmq_url) {
    std::scoped_lock lock(subscriber_mtx_);
    if (zmq_url.empty()) {
        zmq_.clear();
        zmq_url_map_.clear();
    } else if (zmq_url_map_.find(zmq_url) != zmq_url_map_.end()) {
        zmq_.erase(zmq_url_map_[zmq_url]);
        zmq_url_map_.erase(zmq_url);
    }
}

int NodeChannel::send_raw_to_pub(const std::string& raw) {
    return enqueue_send(SendJob{raw, true, false});
}

int NodeChannel::send_raw_to_usr(const std::string& raw) {
    {
        std::scoped_lock lock(state_mtx_);
        if (output_url_.empty()) {
            return -1;
        }
    }
    return enqueue_send(SendJob{raw, false, true});
}

void NodeChannel::set_push_url(const std::string& url) {
    std::scoped_lock lock(state_mtx_);
    output_url_ = url;
}

void NodeChannel::clear_push_url() {
    std::scoped_lock lock(state_mtx_);
    output_url_.clear();
}

int NodeChannel::send(const std::string& object, const nlohmann::json& data,
                      const std::string& error_msg, const std::string& work_id) {
    nlohmann::json out_body;
    bool output_enabled = false;
    {
        std::scoped_lock lock(state_mtx_);
        out_body["request_id"] = request_id_;
        out_body["work_id"] = work_id.empty() ? work_id_ : work_id;
        output_enabled = enoutput_;
    }
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

    const int pub_ret = send_raw_to_pub(out);
    if (output_enabled) {
        return send_raw_to_usr(out);
    }
    return pub_ret;
}

int NodeChannel::send_raw_for_url(const std::string& zmq_url, const std::string& raw) {
    if (zmq_url.empty()) {
        return -1;
    }
    ZmqEndpoint _zmq(zmq_url, ZMQ_PUSH);
    return _zmq.send_data(raw);
}
