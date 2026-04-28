#include "zmq_message.h"

#include <limits>

namespace StackFlows {

namespace {

constexpr char kLongParamMagic0 = '\0';
constexpr char kLongParamMagic1 = static_cast<char>(0xff);
constexpr char kLongParamMagic2 = 'S';
constexpr char kLongParamMagic3 = 'F';
constexpr size_t kLongParamHeaderSize = 8;

void append_uint32_be(std::string& out, uint32_t value) {
    out.push_back(static_cast<char>((value >> 24) & 0xff));
    out.push_back(static_cast<char>((value >> 16) & 0xff));
    out.push_back(static_cast<char>((value >> 8) & 0xff));
    out.push_back(static_cast<char>(value & 0xff));
}

uint32_t read_uint32_be(const char* data) {
    return (static_cast<uint32_t>(static_cast<unsigned char>(data[0])) << 24) |
           (static_cast<uint32_t>(static_cast<unsigned char>(data[1])) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(data[2])) << 8) |
           static_cast<uint32_t>(static_cast<unsigned char>(data[3]));
}

bool is_long_param_packet(const char* data, int size) {
    return size >= static_cast<int>(kLongParamHeaderSize) &&
           data[0] == kLongParamMagic0 &&
           data[1] == kLongParamMagic1 &&
           data[2] == kLongParamMagic2 &&
           data[3] == kLongParamMagic3;
}

}  // namespace

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

    if (size <= 0) return std::string();
    size_t header_size = 1;
    size_t len = static_cast<unsigned char>(data[0]);
    if (is_long_param_packet(data, size)) {
        header_size = kLongParamHeaderSize;
        len = read_uint32_be(data + 4);
    }
    if ((index % 2) == 0) {
        if (header_size + len > static_cast<size_t>(size)) return std::string();
        return std::string(data + header_size, len);
    } else {
        size_t offset = header_size + len;
        if (offset > static_cast<size_t>(size)) return std::string();
        return std::string(data + offset, static_cast<size_t>(size) - offset);
    }
}

std::string ZmqMessage::set_param(std::string_view param0, std::string_view param1) {
    std::string data;
    if (param0.length() <= 255) {
        data.reserve(1 + param0.length() + param1.length());
        data.push_back(static_cast<char>(param0.length()));
        data.append(param0);
        data.append(param1);
        return data;
    }

    if (param0.length() > std::numeric_limits<uint32_t>::max()) {
        return {};
    }
    data.reserve(kLongParamHeaderSize + param0.length() + param1.length());
    data.push_back(kLongParamMagic0);
    data.push_back(kLongParamMagic1);
    data.push_back(kLongParamMagic2);
    data.push_back(kLongParamMagic3);
    append_uint32_be(data, static_cast<uint32_t>(param0.length()));
    data.append(param0);
    data.append(param1);
    return data;
}

}  // namespace StackFlows
