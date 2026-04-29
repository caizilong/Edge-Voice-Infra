#include <array>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <list>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
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
        task.knowledge_db_path = config["vector_db"].get<std::string>();
      }
      if (config.contains("knowledge_db") && config["knowledge_db"].is_string()) {
        task.knowledge_db_path = config["knowledge_db"].get<std::string>();
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
      if (config.contains("candidate_k") && config["candidate_k"].is_number_integer()) {
        task.candidate_k = config["candidate_k"].get<int>();
      }
      if (config.contains("context_chars") && config["context_chars"].is_number_integer()) {
        task.context_chars = config["context_chars"].get<int>();
      }
      if (config.contains("cache_size") && config["cache_size"].is_number_integer()) {
        task.cache_size = config["cache_size"].get<size_t>();
      }
    } catch (...) {
    }
    task.fill_defaults();
    tasks_[work_id_num] = std::move(task);

    channel->subscriber_work_id(
        "", [this, weak_channel = std::weak_ptr<StackFlows::NodeChannel>(channel),
             work_id_num](const std::string& object, const std::string& data) {
          on_inference(work_id_num, weak_channel, object, data);
        });

    nlohmann::json response;
    response["service"] = "rag";
    response["status"] = "ready";
    response["backend"] = "sqlite-fts5-python-vector";
    response["model"] = tasks_[work_id_num].model_path;
    response["knowledge_db"] = tasks_[work_id_num].knowledge_db_path;
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
    std::string knowledge_db_path;
    std::string script_path;
    int top_k = 3;
    int candidate_k = 30;
    int context_chars = 1600;
    double threshold = 0.35;
    size_t cache_size = 32;
    std::list<std::string> cache_order;
    std::unordered_map<std::string,
                       std::pair<nlohmann::json, std::list<std::string>::iterator>>
        response_cache;

    void fill_defaults() {
      if (model_path.empty()) {
        model_path = first_existing_path({
            "models/rag/text2vec-base-chinese",
            "../models/rag/text2vec-base-chinese",
            "/home/pi/Edge-Voice-Infra/models/rag/text2vec-base-chinese",
        });
      }
      if (knowledge_db_path.empty()) {
        knowledge_db_path = first_existing_path({
            "services/rag-service/data/knowledge/vehicle_knowledge.sqlite",
            "../services/rag-service/data/knowledge/vehicle_knowledge.sqlite",
            "/home/pi/Edge-Voice-Infra/services/rag-service/data/knowledge/vehicle_knowledge.sqlite",
        });
      }
      if (script_path.empty()) {
        script_path = first_existing_path({
            "services/rag-service/rag/query.py",
            "../services/rag-service/rag/query.py",
            "/home/pi/Edge-Voice-Infra/services/rag-service/rag/query.py",
        });
      }
    }

    std::string cache_key(const std::string& query) const {
      return query + "\n" + model_path + "\n" + knowledge_db_path + "\n" + script_path +
             "\n" + std::to_string(top_k) + "\n" + std::to_string(candidate_k) + "\n" +
             std::to_string(threshold) + "\n" + std::to_string(context_chars);
    }

    bool get_cached(const std::string& key, nlohmann::json& result) {
      auto it = response_cache.find(key);
      if (it == response_cache.end()) {
        return false;
      }
      cache_order.splice(cache_order.begin(), cache_order, it->second.second);
      result = it->second.first;
      if (result.contains("metrics") && result["metrics"].is_object()) {
        result["metrics"]["cache_hit"] = true;
      }
      return true;
    }

    void put_cached(const std::string& key, const nlohmann::json& result) {
      if (cache_size == 0) {
        return;
      }
      auto it = response_cache.find(key);
      if (it != response_cache.end()) {
        it->second.first = result;
        cache_order.splice(cache_order.begin(), cache_order, it->second.second);
        return;
      }
      cache_order.push_front(key);
      response_cache.emplace(key, std::make_pair(result, cache_order.begin()));
      while (response_cache.size() > cache_size) {
        const std::string& old_key = cache_order.back();
        response_cache.erase(old_key);
        cache_order.pop_back();
      }
    }
  };

  bool run_rag(RagTask& task, const std::string& query, nlohmann::json& result,
               std::string& error_message) {
    const std::string cache_key = task.cache_key(query);
    if (task.get_cached(cache_key, result)) {
      return true;
    }

    std::string query_file;
    if (!write_query_file(query, query_file)) {
      error_message = "failed to create RAG query file";
      return false;
    }

    std::string command = "python3 " + shell_quote(task.script_path) +
                          " --input " + shell_quote(query_file) +
                          " --model " + shell_quote(task.model_path) +
                          " --db " + shell_quote(task.knowledge_db_path) +
                          " --top-k " + std::to_string(task.top_k) +
                          " --candidate-k " + std::to_string(task.candidate_k) +
                          " --threshold " + std::to_string(task.threshold) +
                          " --context-chars " + std::to_string(task.context_chars);
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
    if (result.contains("metrics") && result["metrics"].is_object()) {
      result["metrics"]["cache_hit"] = false;
    }
    task.put_cached(cache_key, result);
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
    response["backend"] = rag_result.is_object()
                              ? rag_result.value("backend", "sqlite-fts5-python-vector")
                              : "file-context";
    response["metrics"] = rag_result.is_object() ? rag_result.value("metrics", nlohmann::json::object())
                                                 : nlohmann::json::object();
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
