#include <csignal>
#include <fstream>
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

std::string extract_text(const std::string& raw) {
  if (raw.empty() || raw == "None") {
    return {};
  }
  try {
    auto body = nlohmann::json::parse(raw);
    if (body.is_string()) {
      return body.get<std::string>();
    }
    if (body.is_object()) {
      std::string text = first_string_field(body, {"text", "prompt", "query", "input"});
      if (!text.empty()) {
        return text;
      }
    }
  } catch (...) {
  }
  return raw;
}

class TtsIpcService : public StackFlows::StackFlow {
 public:
  TtsIpcService() : StackFlow("tts") {}

  int setup(const std::string& work_id, const std::string& object,
            const std::string& data) override {
    (void)object;
    int work_id_num = StackFlows::sample_get_work_id_num(work_id);
    auto channel = get_channel(work_id);
    channel->set_output(true);
    channel->set_stream(false);

    TtsTask task;
    try {
      auto config = nlohmann::json::parse(data.empty() ? "{}" : data);
      if (config.contains("output_dir") && config["output_dir"].is_string()) {
        task.output_dir = config["output_dir"].get<std::string>();
      }
    } catch (...) {
    }
    mkdir(task.output_dir.c_str(), 0777);
    tasks_[work_id_num] = task;

    channel->subscriber_work_id(
        "", [this, weak_channel = std::weak_ptr<StackFlows::NodeChannel>(channel),
             work_id_num](const std::string& object, const std::string& data) {
          on_inference(work_id_num, weak_channel, object, data);
        });

    nlohmann::json response;
    response["service"] = "tts";
    response["status"] = "ready";
    response["backend"] = "phase1-text-artifact";
    response["output_dir"] = tasks_[work_id_num].output_dir;
    send("tts.setup", response, NODE_NO_ERROR, work_id);
    return 0;
  }

  void taskinfo(const std::string& work_id, const std::string& object,
                const std::string& data) override {
    (void)object;
    (void)data;
    nlohmann::json response;
    response["service"] = "tts";
    response["active_tasks"] = tasks_.size();
    send("tts.taskinfo", response, NODE_NO_ERROR, work_id);
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
    send("tts.exit", "ok", NODE_NO_ERROR, work_id);
    return 0;
  }

 private:
  struct TtsTask {
    std::string output_dir = "/tmp/edge_voice_tts";
    int counter = 0;
  };

  void on_inference(int work_id_num,
                    std::weak_ptr<StackFlows::NodeChannel> weak_channel,
                    const std::string& object, const std::string& data) {
    (void)object;
    auto channel = weak_channel.lock();
    if (!channel) {
      return;
    }
    const std::string text = extract_text(data);
    if (text.empty()) {
      nlohmann::json error;
      error["code"] = -24;
      error["message"] = "empty text";
      channel->send("tts.error", "None", error.dump());
      return;
    }

    auto& task = tasks_[work_id_num];
    const std::string artifact =
        task.output_dir + "/tts_phase1_" + std::to_string(++task.counter) + ".txt";
    std::ofstream out(artifact);
    out << text << "\n";
    out.close();

    nlohmann::json response;
    response["text"] = text;
    response["artifact"] = artifact;
    response["mime_type"] = "text/plain";
    response["backend"] = "phase1-text-artifact";
    channel->send("tts.response", response, NODE_NO_ERROR);
  }

  std::unordered_map<int, TtsTask> tasks_;
};

}  // namespace

int main() {
  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);
  mkdir("/tmp/llm", 0777);

  TtsIpcService service;
  while (!g_exit) {
    sleep(1);
  }
  return 0;
}
