#include "remote_action.h"
#include <cstdio>
#include <simdjson.h>
#include <string>
#include <vector>
#include "StackFlowUtil.h"
#include "all.h"
#include "json.hpp"
#include "zmq_endpoint.h"

using namespace StackFlows;

namespace {

std::string format_client_url(int com_id) {
    const int size = std::snprintf(nullptr, 0, zmq_c_format.c_str(), com_id);
    if (size <= 0) {
        return {};
    }
    std::vector<char> buff(static_cast<size_t>(size) + 1, 0);
    std::snprintf(buff.data(), buff.size(), zmq_c_format.c_str(), com_id);
    return std::string(buff.data());
}

}  // namespace

int remote_call(int com_id, const std::string& json_str) {
    simdjson::ondemand::parser parser;
    simdjson::padded_string json_string(json_str);
    simdjson::ondemand::document doc;
    auto error = parser.iterate(json_string).get(doc);
    if (error) {
        return -1;
    }

    std::string work_id;
    error = doc["work_id"].get_string(work_id);
    if (error) {
        return -1;
    }
    std::string work_unit = work_id.substr(0, work_id.find("."));
    std::string action;
    error = doc["action"].get_string(action);
    if (error) {
        return -1;
    }

    if (work_id.empty() || action.empty()) {
        return -1;
    }
    std::string com_url = format_client_url(com_id);
    if (com_url.empty()) {
        return -1;
    }
    std::string param = ZmqMessage::set_param(com_url, json_str);
    if (param.empty()) {
        return -1;
    }
    ZmqEndpoint clent(work_unit);
    // 打包操作：客户端相关url数据
    return clent.call_rpc_action(action, param,
                                 [](ZmqEndpoint* _ZmqEndpoint, const std::shared_ptr<ZmqMessage>& val) {});
}
