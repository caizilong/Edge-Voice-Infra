#include "StackFlowUtil.h"
#include <glob.h>
#include <fstream>
#include <vector>
#include "json.hpp"
#include "zmq_endpoint.h"
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif

std::string StackFlows::sample_json_str_get(const std::string& json_str,
                                            const std::string& json_key) {
    try {
        auto body = nlohmann::json::parse(json_str);
        auto it = body.find(json_key);
        if (it == body.end() || it->is_null()) {
            return {};
        }
        if (it->is_string()) {
            return it->get<std::string>();
        }
        return it->dump();
    } catch (...) {
        return {};
    }
}

int StackFlows::sample_get_work_id_num(const std::string& work_id) {
    int a = work_id.find(".");
    if ((a == std::string::npos) || (a == work_id.length() - 1)) {
        return WORK_ID_NONE;
    }
    try {
        return std::stoi(work_id.substr(a + 1));
    } catch (...) {
        return WORK_ID_NONE;
    }
}

std::string StackFlows::sample_get_work_id_name(const std::string& work_id) {
    int a = work_id.find(".");
    if (a == std::string::npos) {
        return work_id;
    } else {
        return work_id.substr(0, a);
    }
}

std::string StackFlows::sample_get_work_id(int work_id_num, const std::string& unit_name) {
    return unit_name + "." + std::to_string(work_id_num);
}

void StackFlows::unicode_to_utf8(unsigned int codepoint, char* output, int* length) {
    if (codepoint <= 0x7F) {
        output[0] = codepoint & 0x7F;
        *length = 1;
    } else if (codepoint <= 0x7FF) {
        output[0] = 0xC0 | ((codepoint >> 6) & 0x1F);
        output[1] = 0x80 | (codepoint & 0x3F);
        *length = 2;
    } else if (codepoint <= 0xFFFF) {
        output[0] = 0xE0 | ((codepoint >> 12) & 0x0F);
        output[1] = 0x80 | ((codepoint >> 6) & 0x3F);
        output[2] = 0x80 | (codepoint & 0x3F);
        *length = 3;
    } else if (codepoint <= 0x10FFFF) {
        output[0] = 0xF0 | ((codepoint >> 18) & 0x07);
        output[1] = 0x80 | ((codepoint >> 12) & 0x3F);
        output[2] = 0x80 | ((codepoint >> 6) & 0x3F);
        output[3] = 0x80 | (codepoint & 0x3F);
        *length = 4;
    } else {
        *length = 0;
    }
}

bool StackFlows::decode_text_stream(const std::string& in, std::string& out,
                               std::unordered_map<int, std::string>& stream_buff) {
    int index = 0;
    try {
        index = std::stoi(StackFlows::sample_json_str_get(in, "index"));
    } catch (...) {
        return false;
    }
    std::string finish = StackFlows::sample_json_str_get(in, "finish");
    stream_buff[index] = StackFlows::sample_json_str_get(in, "delta");
    if (finish.find("f") == std::string::npos) {
        for (size_t i = 0; i < stream_buff.size(); i++) {
            auto it = stream_buff.find(static_cast<int>(i));
            if (it == stream_buff.end()) {
                return true;
            }
            out += it->second;
        }
        stream_buff.clear();
        return false;
    }
    return true;
}

bool StackFlows::decode_stream(const std::string& in, std::string& out,
                               std::unordered_map<int, std::string>& stream_buff) {
    return decode_text_stream(in, out, stream_buff);
}

std::string StackFlows::unit_call(const std::string& unit_name, const std::string& unit_action,
                                  const std::string& data) {
    std::string value;
    ZmqEndpoint _call(unit_name);
    _call.call_rpc_action(unit_action, data,
                          [&value](ZmqEndpoint* _ZmqEndpoint, const std::shared_ptr<ZmqMessage>& raw) {
                              value = raw->string();
                          });
    return value;
}

void StackFlows::unit_call(
        const std::string& unit_name, const std::string& unit_action, const std::string& data,
        std::function<void(const std::shared_ptr<StackFlows::ZmqMessage>&)> callback) {
    StackFlows::ZmqEndpoint _call(unit_name);
    _call.call_rpc_action(
            unit_action, data,
            [callback](StackFlows::ZmqEndpoint* _ZmqEndpoint,
                       const std::shared_ptr<StackFlows::ZmqMessage>& raw) { callback(raw); });
}

bool StackFlows::file_exists(const std::string& filePath) {
    std::ifstream file(filePath);
    return file.good();
}
