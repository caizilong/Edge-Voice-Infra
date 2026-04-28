#include <array>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
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

std::string first_existing_path(const std::vector<std::string>& candidates) {
  for (const auto& path : candidates) {
    if (std::filesystem::exists(path)) {
      return path;
    }
  }
  return candidates.empty() ? std::string() : candidates.front();
}

std::string shell_quote(const std::string& value) {
  std::string out = "'";
  for (char c : value) {
    if (c == '\'') {
      out += "'\\''";
    } else {
      out += c;
    }
  }
  out += "'";
  return out;
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

std::string run_command_capture_stdout(const std::string& command, int& exit_code) {
  std::string output;
  FILE* pipe = popen(command.c_str(), "r");
  if (pipe == nullptr) {
    exit_code = -1;
    return output;
  }

  std::array<char, 4096> buffer{};
  while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
    output += buffer.data();
  }
  int status = pclose(pipe);
  exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  return output;
}

bool write_query_file(const std::string& query, std::string& path) {
  std::string pattern = "/tmp/edge_voice_rag_query_XXXXXX";
  std::vector<char> writable(pattern.begin(), pattern.end());
  writable.push_back('\0');
  int fd = mkstemp(writable.data());
  if (fd < 0) {
    return false;
  }
  path = writable.data();
  nlohmann::json body;
  body["query"] = query;
  std::string raw = body.dump();
  ssize_t written = write(fd, raw.data(), raw.size());
  close(fd);
  return written == static_cast<ssize_t>(raw.size());
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
      if (config.contains("model") && config["model"].is_string()) {
        task.model_path = config["model"].get<std::string>();
      }
      if (config.contains("vector_db") && config["vector_db"].is_string()) {
        task.vector_db_path = config["vector_db"].get<std::string>();
      }
      if (config.contains("script") && config["script"].is_string()) {
        task.script_path = config["script"].get<std::string>();
      }
      if (config.contains("top_k") && config["top_k"].is_number_integer()) {
        task.top_k = config["top_k"].get<int>();
      }
      if (config.contains("threshold") && config["threshold"].is_number()) {
        task.threshold = config["threshold"].get<double>();
      }
    } catch (...) {
    }
    task.fill_defaults();
    tasks_[work_id_num] = task;

    channel->subscriber_work_id(
        "", [this, weak_channel = std::weak_ptr<StackFlows::NodeChannel>(channel),
             work_id_num](const std::string& object, const std::string& data) {
          on_inference(work_id_num, weak_channel, object, data);
        });

    nlohmann::json response;
    response["service"] = "rag";
    response["status"] = "ready";
    response["backend"] = "sentence-transformers-cli";
    response["model"] = tasks_[work_id_num].model_path;
    response["vector_db"] = tasks_[work_id_num].vector_db_path;
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
    std::string model_path;
    std::string vector_db_path;
    std::string script_path;
    int top_k = 1;
    double threshold = 0.5;

    void fill_defaults() {
      if (model_path.empty()) {
        model_path = first_existing_path({
            "models/rag/text2vec-base-chinese",
            "../models/rag/text2vec-base-chinese",
            "/home/pi/Edge-Voice-Infra/models/rag/text2vec-base-chinese",
        });
      }
      if (vector_db_path.empty()) {
        vector_db_path = first_existing_path({
            "services/rag-service/python/vector_db",
            "../services/rag-service/python/vector_db",
            "/home/pi/Edge-Voice-Infra/services/rag-service/python/vector_db",
        });
      }
      if (script_path.empty()) {
        script_path = first_existing_path({
            "services/rag-service/python/rag_query.py",
            "../services/rag-service/python/rag_query.py",
            "/home/pi/Edge-Voice-Infra/services/rag-service/python/rag_query.py",
        });
      }
    }
  };

  bool run_rag(const RagTask& task, const std::string& query, nlohmann::json& result,
               std::string& error_message) {
    std::string query_file;
    if (!write_query_file(query, query_file)) {
      error_message = "failed to create RAG query file";
      return false;
    }

    std::string command = "python3 " + shell_quote(task.script_path) +
                          " --input " + shell_quote(query_file) +
                          " --model " + shell_quote(task.model_path) +
                          " --vector-db " + shell_quote(task.vector_db_path) +
                          " --top-k " + std::to_string(task.top_k) +
                          " --threshold " + std::to_string(task.threshold);
    int exit_code = 0;
    std::string output = run_command_capture_stdout(command, exit_code);
    unlink(query_file.c_str());

    try {
      result = nlohmann::json::parse(output);
    } catch (...) {
      error_message = "RAG backend returned invalid JSON";
      return false;
    }
    if (exit_code != 0 || result.contains("error")) {
      error_message = result.contains("error") ? result["error"].dump() : "RAG backend failed";
      return false;
    }
    return true;
  }

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
    nlohmann::json rag_result;
    if (it != tasks_.end()) {
      if (!it->second.knowledge.empty()) {
        context = it->second.knowledge;
      } else {
        std::string error_message;
        if (!run_rag(it->second, query, rag_result, error_message)) {
          nlohmann::json error;
          error["code"] = -31;
          error["message"] = error_message;
          channel->send("rag.error", "None", error.dump());
          return;
        }
        context = rag_result.value("context", context);
      }
    }

    nlohmann::json response;
    response["query"] = query;
    response["classification"] = query_type;
    response["context"] = context;
    response["prompt"] = "Question: " + query + "\nContext: " + context;
    response["results"] = rag_result.is_object() ? rag_result.value("results", nlohmann::json::array())
                                                 : nlohmann::json::array();
    response["backend"] = rag_result.is_object() ? "sentence-transformers-cli" : "file-context";
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
