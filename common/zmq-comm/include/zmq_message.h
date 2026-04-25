#pragma once
#include <zmq.h>
#include <memory>
#include <string>
#include <string_view>

namespace StackFlows {
class ZmqMessage {
private:
    zmq_msg_t msg;

public:
    ZmqMessage();
    ~ZmqMessage();

    ZmqMessage(const ZmqMessage&) = delete;
    ZmqMessage& operator=(const ZmqMessage&) = delete;

    ZmqMessage(ZmqMessage&& other) noexcept;
    ZmqMessage& operator=(ZmqMessage&& other) noexcept;

    std::shared_ptr<std::string> get_string();
    std::string string();
    
    [[nodiscard]] std::string_view view() const noexcept;

    void *data();
    [[nodiscard]] size_t size() const;
    zmq_msg_t *get();

    std::string get_param(int index, std::string_view idata = "");
    static std::string set_param(std::string_view param0, std::string_view param1);
};
}  // namespace StackFlows