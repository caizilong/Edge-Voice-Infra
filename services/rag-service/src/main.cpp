#include <csignal>
#include <fstream>
#include <iostream>
#include <sstream>
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

std::string json_text_field(const nlohmann::json& body,
                            const std::vector<std::string>& keys) {
  for (const auto& key : keys) {
    auto it = body.find(key);
    if (it != body.end() && it->is_string()) {
      return it->get<std::string>();
    }
  }
  return {};
}

std::string extract_text_payload(const std::string& raw) {
  if (raw.empty() || raw == "None") {
    return {};
  }
  try {
    auto body = nlohmann::json::parse(raw);
    if (body.is_string()) {
      return body.get<std::string>();
    }
    if (body.is_object()) {
      std::string value = json_text_field(body, {"query", "text", "prompt", "input"});
      if (!value.empty()) {
        return value;
      }
    }
  } catch (...) {
  }
  return raw;
}

std::string classify_query(const std::string& query) {
  static const std::vector<std::string> emergency = {
      "fault", "warning", "danger", "brake", "engine", "emergency",
      "故障", "警告", "危险", "制动", "发动机", "紧急"};
  static const std::vector<std::string> maintenance = {
      "maintenance", "service", "oil", "tire", "battery",
      "保养", "维修", "机油", "轮胎", "电瓶"};
  for (const auto& key : emergency) {
    if (query.find(key) != std::string::npos) {
      return "emergency";
    }
  }
  for (const auto& key : maintenance) {
    if (query.find(key) != std::string::npos) {
      return "maintenance";
    }
  }
  return "general";
}

std::string default_context(const std::string& query_type) {
  if (query_type == "emergency") {
    return "Vehicle emergency guidance: keep the vehicle stable, reduce speed, stop in a safe place, and check warning indicators before continuing.";
  }
  if (query_type == "maintenance") {
    return "Vehicle maintenance guidance: follow the scheduled service interval, check oil, coolant, brake fluid, tires, and battery condition.";
  }
  return "Vehicle assistant context: answer with concise, safety-aware driving and maintenance guidance.";
}

std::string load_text_file(const std::string& path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    return {};
  }
  std::ostringstream out;
  out << file.rdbuf();
  return out.str();
}

class RagService : public StackFlows::StackFlow {
 public:
  RagService() : StackFlow("rag") {}

  int setup(const std::string& work_id, const std::string& object,
            const std::string& data) override {
    (void)object;
    int work_id_num = StackFlows::sample_get_work_id_num(work_id);
    auto channel = get_channel(work_id);
    channel->set_output(true);
    channel->set_stream(false);

    RagTask task;
    try {
      auto config = nlohmann::json::parse(data.empty() ? "{}" : data);
      if (config.contains("knowledge_file") && config["knowledge_file"].is_string()) {
        task.knowledge = load_text_file(config["knowledge_file"].get<std::string>());
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
    response["service"] = "rag";
    response["status"] = "ready";
    send("rag.setup", response, NODE_NO_ERROR, work_id);
    return 0;
  }

  void taskinfo(const std::string& work_id, const std::string& object,
                const std::string& data) override {
    (void)object;
    (void)data;
    nlohmann::json response;
    response["service"] = "rag";
    response["active_tasks"] = tasks_.size();
    send("rag.taskinfo", response, NODE_NO_ERROR, work_id);
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
    send("rag.exit", "ok", NODE_NO_ERROR, work_id);
    return 0;
  }

 private:
  struct RagTask {
    std::string knowledge;
  };

  void on_inference(int work_id_num,
                    std::weak_ptr<StackFlows::NodeChannel> weak_channel,
                    const std::string& object, const std::string& data) {
    (void)object;
    auto channel = weak_channel.lock();
    if (!channel) {
      return;
    }

    const std::string query = extract_text_payload(data);
    if (query.empty()) {
      nlohmann::json error;
      error["code"] = -24;
      error["message"] = "empty query";
      channel->send("rag.error", "None", error.dump());
      return;
    }

    const std::string query_type = classify_query(query);
    std::string context = default_context(query_type);
    auto it = tasks_.find(work_id_num);
    if (it != tasks_.end() && !it->second.knowledge.empty()) {
      context = it->second.knowledge;
    }

    nlohmann::json response;
    response["query"] = query;
    response["classification"] = query_type;
    response["context"] = context;
    response["prompt"] = "Question: " + query + "\nContext: " + context;
    channel->send("rag.response", response, NODE_NO_ERROR);
  }

  std::unordered_map<int, RagTask> tasks_;
};

}  // namespace

int main() {
  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);
  mkdir("/tmp/llm", 0777);

  RagService service;
  while (!g_exit) {
    sleep(1);
  }
  return 0;
}
