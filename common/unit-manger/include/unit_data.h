#pragma once

#include <vector>
#include "zmq_endpoint.h"

using namespace StackFlows;

class unit_data {
private:
    std::unique_ptr<ZmqEndpoint> user_inference_chennal_;

public:
    std::string work_id;
    std::string output_url;
    std::string inference_url;
    int port_;

    unit_data();
    void init_zmq(const std::string& url);
    void send_msg(const std::string& json_str);
    ~unit_data();
};
