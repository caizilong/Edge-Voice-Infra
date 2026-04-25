#include "zmq_message.h"

namespace StackFlows {

ZmqMessage::ZmqMessage() { zmq_msg_init(&msg); }

ZmqMessage::~ZmqMessage() { zmq_msg_close(&msg); }

ZmqMessage::ZmqMessage(ZmqMessage&& other) noexcept {
    zmq_msg_init(&msg);
    zmq_msg_move(&msg, &other.msg);
}

ZmqMessage& ZmqMessage::operator=(ZmqMessage&& other) noexcept {
    if (this != &other) {
        zmq_msg_close(&msg);
        zmq_msg_init(&msg);
        zmq_msg_move(&msg, &other.msg);
    }
    return *this;
}

std::shared_ptr<std::string> ZmqMessage::get_string() {
    return std::make_shared<std::string>(static_cast<const char*>(zmq_msg_data(&msg)), zmq_msg_size(&msg));
}

std::string ZmqMessage::string() {
    return std::string(static_cast<const char*>(zmq_msg_data(&msg)), zmq_msg_size(&msg));
}

std::string_view ZmqMessage::view() const noexcept {
    return {static_cast<const char*>(zmq_msg_data(const_cast<zmq_msg_t*>(&msg))), zmq_msg_size(const_cast<zmq_msg_t*>(&msg))};
}

void* ZmqMessage::data() { return zmq_msg_data(&msg); }

size_t ZmqMessage::size() const { return zmq_msg_size(const_cast<zmq_msg_t*>(&msg)); }

zmq_msg_t* ZmqMessage::get() { return &msg; }

std::string ZmqMessage::get_param(int index, std::string_view idata) {
    const char* data = nullptr;
    int size = 0;

    if (!idata.empty()) {
        data = idata.data();
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

std::string ZmqMessage::set_param(std::string_view param0, std::string_view param1) {
    std::string data;
    data.reserve(1 + param0.length() + param1.length());
    data.push_back(static_cast<char>(param0.length()));
    data.append(param0);
    data.append(param1);
    return data;
}

}  // namespace StackFlows