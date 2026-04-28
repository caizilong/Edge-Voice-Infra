#include <csignal>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "StackFlow.h"
#include "json.hpp"

namespace {

volatile sig_atomic_t g_exit = 0;

void handle_signal(int) {
  g_exit = 1;
}

std::string first_string_field(const nlohmann::json& body,
                               const std::vector<std::string>& keys) {
  for (const auto& key : keys) {
    auto it = body.find(key);
    if (it != body.end() && it->is_string()) {
      return it->get<std::string>();
    }
  }
  return {};
}

std::string extract_prompt(const std::string& raw) {
  if (raw.empty() || raw == "None") {
    return {};
  }
  try {
    auto body = nlohmann::json::parse(raw);
    if (body.is_string()) {
      return body.get<std::string>();
    }
    if (body.is_object()) {
      std::string prompt = first_string_field(body, {"prompt", "query", "text", "input"});
      if (!prompt.empty()) {
        return prompt;
      }
    }
  } catch (...) {
  }
  return raw;
}

class LlmIpcService : public StackFlows::StackFlow {
 public:
  LlmIpcService() : StackFlow("llm") {}

  int setup(const std::string& work_id, const std::string& object,
            const std::string& data) override {
    (void)object;
    int work_id_num = StackFlows::sample_get_work_id_num(work_id);
    auto channel = get_channel(work_id);
    channel->set_output(true);
    channel->set_stream(false);

    LlmTask task;
    try {
      auto config = nlohmann::json::parse(data.empty() ? "{}" : data);
      if (config.contains("model") && config["model"].is_string()) {
        task.model = config["model"].get<std::string>();
      }
    } catch (...) {
    }
    tasks_[work_id_num] = task;

    channel->subscriber_work_id(
        "", [this, weak_channel = std::weak_ptr<StackFlows::NodeChannel>(channel),
             work_id_num](const std::string& object, const std::string& data) {
          on_inference(work_id_num, weak_channel, object, data);
        });

    nlohmann::json response;
    response["service"] = "llm";
    response["status"] = "ready";
    response["backend"] = "phase1-mock";
    response["model"] = tasks_[work_id_num].model;
    send("llm.setup", response, NODE_NO_ERROR, work_id);
    return 0;
  }

  void taskinfo(const std::string& work_id, const std::string& object,
                const std::string& data) override {
    (void)object;
    (void)data;
    nlohmann::json response;
    response["service"] = "llm";
    response["active_tasks"] = tasks_.size();
    send("llm.taskinfo", response, NODE_NO_ERROR, work_id);
  }

  int exit(const std::string& work_id, const std::string& object,
           const std::string& data) override {
    (void)object;
    (void)data;
    int work_id_num = StackFlows::sample_get_work_id_num(work_id);
    auto it = tasks_.find(work_id_num);
    if (it != tasks_.end()) {
      if (auto channel = get_channel(work_id_num)) {
        channel->stop_subscriber_work_id("");
      }
      tasks_.erase(it);
    }
    send("llm.exit", "ok", NODE_NO_ERROR, work_id);
    return 0;
  }

 private:
  struct LlmTask {
    std::string model = "phase1-mock";
  };

  void on_inference(int work_id_num,
                    std::weak_ptr<StackFlows::NodeChannel> weak_channel,
                    const std::string& object, const std::string& data) {
    (void)object;
    auto channel = weak_channel.lock();
    if (!channel) {
      return;
    }
    const std::string prompt = extract_prompt(data);
    if (prompt.empty()) {
      nlohmann::json error;
      error["code"] = -24;
      error["message"] = "empty prompt";
      channel->send("llm.error", "None", error.dump());
      return;
    }

    const auto it = tasks_.find(work_id_num);
    const std::string model = it == tasks_.end() ? "phase1-mock" : it->second.model;

    nlohmann::json response;
    response["model"] = model;
    response["text"] = "Phase1 LLM response: " + prompt;
    response["finish"] = true;
    channel->send("llm.response", response, NODE_NO_ERROR);
  }

  std::unordered_map<int, LlmTask> tasks_;
};

}  // namespace

int main() {
  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);
  mkdir("/tmp/llm", 0777);

  LlmIpcService service;
  while (!g_exit) {
    sleep(1);
  }
  return 0;
}
