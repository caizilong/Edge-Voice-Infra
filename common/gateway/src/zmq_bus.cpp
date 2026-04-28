#include "zmq_bus.h"

#include <StackFlowUtil.h>
#include <stdbool.h>
#include <cstdio>
#include <cstring>
#include <functional>
#include <vector>
#include "all.h"
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif

#ifdef ENABLE_BSON
#include <bson/bson.h>
#endif

using namespace StackFlows;

namespace {

std::string format_zmq_url(const std::string& format, int port) {
    const int size = std::snprintf(nullptr, 0, format.c_str(), port);
    if (size <= 0) {
        return {};
    }
    std::vector<char> buff(static_cast<size_t>(size) + 1, 0);
    std::snprintf(buff.data(), buff.size(), format.c_str(), port);
    return std::string(buff.data());
}

}  // namespace

zmq_bus_com::zmq_bus_com() {
    exit_flage = 1;
    err_count = 0;
    json_str_flage_ = 0;
}

void zmq_bus_com::work(const std::string& zmq_url_format, int port) {
    _port = port;
    exit_flage = 1;
    _zmq_url = format_zmq_url(zmq_url_format, port);
    user_chennal_ = std::make_unique<ZmqEndpoint>(
            _zmq_url, ZMQ_PULL, [this](ZmqEndpoint* _ZmqEndpoint, const std::shared_ptr<ZmqMessage>& data) {
                this->send_data(data->string());
            });
}

void zmq_bus_com::stop() {
    exit_flage = 0;
    user_chennal_.reset();
}

void zmq_bus_com::on_data(const std::string& data) {
    std::cout << "on_data:" << data << std::endl;

    unit_action_match(_port, data);
}

void zmq_bus_com::send_data(const std::string& data) {}

zmq_bus_com::~zmq_bus_com() {
    if (exit_flage) {
        stop();
    }
}

int zmq_bus_publisher_push(const std::string& work_id, const std::string& json_str) {
    ALOGW("zmq_bus_publisher_push json_str:%s", json_str.c_str());

    if (work_id.empty()) {
        ALOGW("work_id is empty");
        return -1;
    }
    unit_data* unit_p = nullptr;
    SAFE_READING(unit_p, unit_data*, work_id);
    if (unit_p) {
        unit_p->send_msg(json_str);
        ALOGW("zmq_bus_publisher_push work_id:%s", work_id.c_str());
    } else {
        ALOGW("zmq_bus_publisher_push failed, not have work_id:%s", work_id.c_str());
        return -1;
    }
    return 0;
}

void* usr_context;

void zmq_com_send(int com_id, const std::string& out_str) {
    std::string zmq_push_url = format_zmq_url(zmq_c_format, com_id);
    if (zmq_push_url.empty()) {
        ALOGE("format zmq push url failed, com_id:%d", com_id);
        return;
    }
    ZmqEndpoint _zmq(zmq_push_url, ZMQ_PUSH);
    std::string out = out_str + "\n";
    _zmq.send_data(out);
}

void zmq_bus_com::select_json_str(const std::string& json_src,
                                  std::function<void(const std::string&)> out_fun) {
    std::string test_json = json_src;

    if (!test_json.empty() && test_json.back() == '\n') {
        test_json.pop_back();
    }
    out_fun(test_json);
}
