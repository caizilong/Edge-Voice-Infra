#include "remote_server.h"
#include <StackFlowUtil.h>
#include <simdjson.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "all.h"
#include "json.hpp"
#include "remote_action.h"
#include "zmq_bus.h"

using namespace StackFlows;

std::atomic<int> work_id_number_counter;
int port_list_start;
std::vector<bool> port_list;
std::unique_ptr<ZmqEndpoint> sys_rpc_server_;

namespace {

int allocate_zmq_port() {
    for (size_t i = 0; i < port_list.size(); i++) {
        if (!port_list[i]) {
            port_list[i] = true;
            return port_list_start + static_cast<int>(i);
        }
    }
    return -1;
}

void release_zmq_port(int port) {
    const int index = port - port_list_start;
    if (index >= 0 && static_cast<size_t>(index) < port_list.size()) {
        port_list[static_cast<size_t>(index)] = false;
    }
}

std::string format_zmq_url(const std::string& unit, const char* suffix, int port) {
    std::string zmq_format = zmq_s_format;
    if (zmq_s_format.find("sock") != std::string::npos) {
        zmq_format += ".";
        zmq_format += unit;
        zmq_format += suffix;
    }

    const int size = std::snprintf(nullptr, 0, zmq_format.c_str(), port);
    if (size <= 0) {
        return {};
    }
    std::vector<char> buff(static_cast<size_t>(size) + 1, 0);
    std::snprintf(buff.data(), buff.size(), zmq_format.c_str(), port);
    return std::string(buff.data());
}

std::string format_zmq_url(const std::string& zmq_format, int port) {
    const int size = std::snprintf(nullptr, 0, zmq_format.c_str(), port);
    if (size <= 0) {
        return {};
    }
    std::vector<char> buff(static_cast<size_t>(size) + 1, 0);
    std::snprintf(buff.data(), buff.size(), zmq_format.c_str(), port);
    return std::string(buff.data());
}

bool parse_zmq_port(const std::string& url, int& port) {
    return std::sscanf(url.c_str(), zmq_s_format.c_str(), &port) == 1;
}

}  // namespace

std::string sys_sql_select(const std::string& key) {
    std::string out;
    SAFE_READING(out, std::string, key);
    return out;
}

void sys_sql_set(const std::string& key, const std::string& val) { SAFE_SETTING(key, val); }

void sys_sql_unset(const std::string& key) { SAFE_ERASE(key); }

unit_data* sys_allocate_unit(const std::string& unit) {
    std::unique_ptr<unit_data> unit_p = std::make_unique<unit_data>();
    {
        unit_p->port_ = work_id_number_counter++;
        std::string ports = std::to_string(unit_p->port_);
        unit_p->work_id = unit + "." + ports;
    }
    int output_port = -1;
    int inference_port = -1;
    {
        output_port = allocate_zmq_port();
        if (output_port < 0) {
            ALOGE("no available zmq output port for unit:%s", unit.c_str());
            return nullptr;
        }
        std::string zmq_s_url = format_zmq_url(unit, ".output_url", output_port);
        if (zmq_s_url.empty()) {
            release_zmq_port(output_port);
            ALOGE("format zmq output url failed for unit:%s", unit.c_str());
            return nullptr;
        }
        unit_p->output_url = zmq_s_url;
    }

    {
        inference_port = allocate_zmq_port();
        if (inference_port < 0) {
            release_zmq_port(output_port);
            ALOGE("no available zmq inference port for unit:%s", unit.c_str());
            return nullptr;
        }
        std::string zmq_s_url = format_zmq_url(unit, ".inference_url", inference_port);
        if (zmq_s_url.empty()) {
            release_zmq_port(output_port);
            release_zmq_port(inference_port);
            ALOGE("format zmq inference url failed for unit:%s", unit.c_str());
            return nullptr;
        }
        unit_p->init_zmq(zmq_s_url);
    }
    unit_data* raw_unit = unit_p.get();
    SAFE_SETTING(unit_p->work_id, raw_unit);
    SAFE_SETTING(unit_p->work_id + ".out_port", unit_p->output_url);
    return unit_p.release();
}

int sys_release_unit(const std::string& unit) {
    unit_data* unit_p = nullptr;
    SAFE_READING(unit_p, unit_data*, unit);
    if (unit_p == nullptr) {
        return -1;
    }

    int port;
    if (parse_zmq_port(unit_p->output_url, port)) {
        release_zmq_port(port);
    }
    if (parse_zmq_port(unit_p->inference_url, port)) {
        release_zmq_port(port);
    }

    delete unit_p;
    SAFE_ERASE(unit);
    SAFE_ERASE(unit + ".out_port");
    return 0;
}

std::string rpc_allocate_unit(ZmqEndpoint* _ZmqEndpoint, const std::shared_ptr<ZmqMessage>& raw) {
    unit_data* unit_info = sys_allocate_unit(raw->string());
    if (unit_info == nullptr) {
        return "False";
    }
    return ZmqMessage::set_param(
            std::to_string(unit_info->port_),
            ZmqMessage::set_param(unit_info->output_url, unit_info->inference_url));
}

std::string rpc_release_unit(ZmqEndpoint* _ZmqEndpoint, const std::shared_ptr<ZmqMessage>& raw) {
    sys_release_unit(raw->string());
    return "Success";
}

std::string rpc_sql_select(ZmqEndpoint* _ZmqEndpoint, const std::shared_ptr<ZmqMessage>& raw) {
    return sys_sql_select(raw->string());
}

std::string rpc_sql_set(ZmqEndpoint* _ZmqEndpoint, const std::shared_ptr<ZmqMessage>& raw) {
    std::string key = sample_json_str_get(raw->string(), "key");
    std::string val = sample_json_str_get(raw->string(), "val");
    if (key.empty()) return "False";
    sys_sql_set(key, val);
    return "Success";
}

std::string rpc_sql_unset(ZmqEndpoint* _ZmqEndpoint, const std::shared_ptr<ZmqMessage>& raw) {
    sys_sql_unset(raw->string());
    return "Success";
}

void remote_server_work() {
    int port_list_end;
    SAFE_READING(work_id_number_counter, int, "config_work_id");
    SAFE_READING(port_list_start, int, "config_zmq_min_port");
    SAFE_READING(port_list_end, int, "config_zmq_max_port");
    if (port_list_end <= port_list_start) {
        ALOGE("invalid zmq port range:%d-%d", port_list_start, port_list_end);
        port_list.clear();
    } else {
        port_list.assign(static_cast<size_t>(port_list_end - port_list_start), false);
    }

    sys_rpc_server_ = std::make_unique<ZmqEndpoint>("sys");
    sys_rpc_server_->register_rpc_action("sql_select", rpc_sql_select);
    sys_rpc_server_->register_rpc_action("register_unit", rpc_allocate_unit);
    sys_rpc_server_->register_rpc_action("release_unit", rpc_release_unit);
    sys_rpc_server_->register_rpc_action("sql_set", rpc_sql_set);
    sys_rpc_server_->register_rpc_action("sql_unset", rpc_sql_unset);
}

void remote_server_stop_work() { sys_rpc_server_.reset(); }

void usr_print_error(const std::string& request_id, const std::string& work_id,
                     const std::string& error_msg, int zmq_out) {
    nlohmann::json out_body;
    out_body["request_id"] = request_id;
    out_body["work_id"] = work_id;
    out_body["created"] = time(nullptr);
    out_body["error"] = nlohmann::json::parse(error_msg);
    out_body["object"] = std::string("None");
    out_body["data"] = std::string("None");
    std::string out = out_body.dump();
    zmq_com_send(zmq_out, out);
}

std::mutex unit_action_match_mtx;
simdjson::ondemand::parser parser;
typedef int (*sys_fun_call)(int, const nlohmann::json&);

void unit_action_match(int com_id, const std::string& json_str) {
    std::lock_guard<std::mutex> guard(unit_action_match_mtx);
    simdjson::padded_string json_string(json_str);
    simdjson::ondemand::document doc;
    auto error = parser.iterate(json_string).get(doc);

    ALOGI("json format :%s", json_str.c_str());

    if (error) {
        ALOGE("json format error:%s", json_str.c_str());
        usr_print_error("0", "sys", "{\"code\":-2, \"message\":\"json format error\"}", com_id);
        return;
    }
    std::string request_id;
    error = doc["request_id"].get_string(request_id);
    if (error) {
        ALOGE("miss request_id, error:%s", simdjson::error_message(error));
        usr_print_error("0", "sys", "{\"code\":-2, \"message\":\"json format error\"}", com_id);
        return;
    }
    std::string work_id;
    error = doc["work_id"].get_string(work_id);
    if (error) {
        ALOGE("miss work_id, error:%s", simdjson::error_message(error));
        usr_print_error("0", "sys", "{\"code\":-2, \"message\":\"json format error\"}", com_id);
        return;
    }
    if (work_id.empty()) work_id = "sys";
    std::string action;
    error = doc["action"].get_string().get(action);
    if (error) {
        ALOGE("miss action, error:%s", simdjson::error_message(error));
        usr_print_error("0", "sys", "{\"code\":-2, \"message\":\"json format error\"}", com_id);
        return;
    }

    std::vector<std::string> work_id_fragment;
    std::string fragment;
    for (auto c : work_id) {
        if (c != '.') {
            fragment.push_back(c);
        } else {
            work_id_fragment.push_back(fragment);
            fragment.clear();
        }
    }
    if (fragment.length()) work_id_fragment.push_back(fragment);
    if (action == "inference") {
        std::string zmq_push_url = format_zmq_url(zmq_c_format, com_id);
        if (zmq_push_url.empty()) {
            usr_print_error(request_id, work_id,
                            "{\"code\":-4, \"message\":\"zmq url format false\"}", com_id);
            return;
        }
        std::string inference_raw_data;
        inference_raw_data.reserve(zmq_push_url.size() + json_str.size() + 13);
        inference_raw_data += "{\"zmq_com\":\"";
        inference_raw_data += zmq_push_url;
        inference_raw_data += "\",";
        inference_raw_data.append(json_str.data() + 1, json_str.length() - 1);
        int ret = zmq_bus_publisher_push(work_id, inference_raw_data);
        if (ret) {
            usr_print_error(request_id, work_id,
                            "{\"code\":-4, \"message\":\"inference data push false\"}", com_id);
        }
    } else {
        if ((work_id_fragment[0].length() != 0) && (remote_call(com_id, json_str) != 0)) {
            usr_print_error(request_id, work_id, "{\"code\":-9, \"message\":\"unit call false\"}",
                            com_id);
        }
    }
}
