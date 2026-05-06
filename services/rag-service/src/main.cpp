#include <array>
#include <cerrno>
#include <csignal>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <list>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
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

bool write_all(int fd, const std::string& data) {
  const char* ptr = data.data();
  size_t remaining = data.size();
  while (remaining > 0) {
    ssize_t written = write(fd, ptr, remaining);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    if (written == 0) {
      return false;
    }
    ptr += written;
    remaining -= static_cast<size_t>(written);
  }
  return true;
}

class RagWorker {
 public:
  RagWorker(std::string script_path, std::string model_path, std::string knowledge_db_path,
            int top_k, int candidate_k, double threshold, int context_chars)
      : script_path_(std::move(script_path)),
        model_path_(std::move(model_path)),
        knowledge_db_path_(std::move(knowledge_db_path)),
        top_k_(top_k),
        candidate_k_(candidate_k),
        threshold_(threshold),
        context_chars_(context_chars) {}

  RagWorker(const RagWorker&) = delete;
  RagWorker& operator=(const RagWorker&) = delete;

  ~RagWorker() {
    stop();
  }

  bool start(std::string& error_message) {
    return ensure_started(error_message);
  }

  bool request(const std::string& query, nlohmann::json& result, std::string& error_message) {
    if (!ensure_started(error_message)) {
      return false;
    }
    if (!send_request(query, result, error_message)) {
      stop();
      if (!ensure_started(error_message)) {
        return false;
      }
      return send_request(query, result, error_message);
    }
    return true;
  }

 private:
  bool ensure_started(std::string& error_message) {
    if (pid_ > 0) {
      int status = 0;
      pid_t ret = waitpid(pid_, &status, WNOHANG);
      if (ret == 0) {
        return true;
      }
      cleanup_handles();
      pid_ = -1;
    }

    int to_child[2] = {-1, -1};
    int from_child[2] = {-1, -1};
    if (pipe(to_child) != 0 || pipe(from_child) != 0) {
      error_message = std::string("failed to create RAG worker pipes: ") + std::strerror(errno);
      if (to_child[0] >= 0) close(to_child[0]);
      if (to_child[1] >= 0) close(to_child[1]);
      if (from_child[0] >= 0) close(from_child[0]);
      if (from_child[1] >= 0) close(from_child[1]);
      return false;
    }

    pid_t child = fork();
    if (child < 0) {
      error_message = std::string("failed to fork RAG worker: ") + std::strerror(errno);
      close(to_child[0]);
      close(to_child[1]);
      close(from_child[0]);
      close(from_child[1]);
      return false;
    }

    if (child == 0) {
      dup2(to_child[0], STDIN_FILENO);
      dup2(from_child[1], STDOUT_FILENO);
      close(to_child[0]);
      close(to_child[1]);
      close(from_child[0]);
      close(from_child[1]);

      const std::string top_k = std::to_string(top_k_);
      const std::string candidate_k = std::to_string(candidate_k_);
      const std::string threshold = std::to_string(threshold_);
      const std::string context_chars = std::to_string(context_chars_);
      execlp("python3", "python3",
             script_path_.c_str(),
             "--worker",
             "--model", model_path_.c_str(),
             "--db", knowledge_db_path_.c_str(),
             "--top-k", top_k.c_str(),
             "--candidate-k", candidate_k.c_str(),
             "--threshold", threshold.c_str(),
             "--context-chars", context_chars.c_str(),
             static_cast<char*>(nullptr));
      _exit(127);
    }

    close(to_child[0]);
    close(from_child[1]);
    write_fd_ = to_child[1];
    read_file_ = fdopen(from_child[0], "r");
    if (read_file_ == nullptr) {
      error_message = std::string("failed to open RAG worker stdout: ") + std::strerror(errno);
      close(write_fd_);
      write_fd_ = -1;
      close(from_child[0]);
      kill(child, SIGTERM);
      waitpid(child, nullptr, 0);
      return false;
    }
    pid_ = child;
    return true;
  }

  bool send_request(const std::string& query, nlohmann::json& result, std::string& error_message) {
    nlohmann::json payload;
    payload["query"] = query;
    std::string raw = payload.dump();
    raw.push_back('\n');
    if (!write_all(write_fd_, raw)) {
      error_message = std::string("failed to write RAG worker request: ") + std::strerror(errno);
      return false;
    }

    std::string line;
    std::array<char, 4096> buffer{};
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), read_file_) != nullptr) {
      line += buffer.data();
      if (!line.empty() && line.back() == '\n') {
        break;
      }
    }
    if (line.empty()) {
      error_message = "RAG worker closed stdout";
      return false;
    }

    try {
      result = nlohmann::json::parse(line);
    } catch (const std::exception& exc) {
      error_message = std::string("RAG worker returned invalid JSON: ") + exc.what();
      return false;
    }
    return true;
  }

  void cleanup_handles() {
    if (read_file_ != nullptr) {
      fclose(read_file_);
      read_file_ = nullptr;
    }
    if (write_fd_ >= 0) {
      close(write_fd_);
      write_fd_ = -1;
    }
  }

  void stop() {
    cleanup_handles();
    if (pid_ > 0) {
      int status = 0;
      pid_t ret = waitpid(pid_, &status, WNOHANG);
      if (ret == 0) {
        kill(pid_, SIGTERM);
        for (int i = 0; i < 20; ++i) {
          ret = waitpid(pid_, &status, WNOHANG);
          if (ret == pid_) {
            break;
          }
          usleep(50000);
        }
        if (ret == 0) {
          kill(pid_, SIGKILL);
          waitpid(pid_, &status, 0);
        }
      }
      pid_ = -1;
    }
  }

  std::string script_path_;
  std::string model_path_;
  std::string knowledge_db_path_;
  int top_k_;
  int candidate_k_;
  double threshold_;
  int context_chars_;
  pid_t pid_ = -1;
  int write_fd_ = -1;
  FILE* read_file_ = nullptr;
};

class RagService : public StackFlows::StackFlow {
 public:
  RagService() : StackFlow("rag") {
    worker_ = std::jthread([this](std::stop_token stoken) { worker_loop(stoken); });
  }

  ~RagService() override {
    {
      std::scoped_lock lock(queue_mutex_);
      stopping_ = true;
      inference_queue_.clear();
    }
    queue_cv_.notify_all();
    if (worker_.joinable()) {
      worker_.request_stop();
      worker_.join();
    }
  }

  int setup(const std::string& work_id, const std::string& object,
            const std::string& data) override {
    (void)object;
    int work_id_num = StackFlows::sample_get_work_id_num(work_id);
    auto channel = get_channel(work_id);
    if (!channel) {
      send("rag.error", "missing channel", NODE_NO_ERROR, work_id);
      return -1;
    }
    channel->set_output(true);
    channel->set_stream(false);

    auto task = std::make_shared<RagTask>();
    try {
      auto config = nlohmann::json::parse(data.empty() ? "{}" : data);
      if (config.contains("knowledge_file") && config["knowledge_file"].is_string()) {
        task->knowledge = load_text_file(config["knowledge_file"].get<std::string>());
      }
      if (config.contains("model") && config["model"].is_string()) {
        task->model_path = config["model"].get<std::string>();
      }
      if (config.contains("vector_db") && config["vector_db"].is_string()) {
        task->knowledge_db_path = config["vector_db"].get<std::string>();
      }
      if (config.contains("knowledge_db") && config["knowledge_db"].is_string()) {
        task->knowledge_db_path = config["knowledge_db"].get<std::string>();
      }
      if (config.contains("script") && config["script"].is_string()) {
        task->script_path = config["script"].get<std::string>();
      }
      if (config.contains("top_k") && config["top_k"].is_number_integer()) {
        task->top_k = config["top_k"].get<int>();
      }
      if (config.contains("threshold") && config["threshold"].is_number()) {
        task->threshold = config["threshold"].get<double>();
      }
      if (config.contains("candidate_k") && config["candidate_k"].is_number_integer()) {
        task->candidate_k = config["candidate_k"].get<int>();
      }
      if (config.contains("context_chars") && config["context_chars"].is_number_integer()) {
        task->context_chars = config["context_chars"].get<int>();
      }
      if (config.contains("cache_size") && config["cache_size"].is_number_integer()) {
        task->cache_size = config["cache_size"].get<size_t>();
      }
    } catch (...) {
    }
    task->fill_defaults();
    if (task->knowledge.empty()) {
      std::string error_message;
      if (!task->get_worker().start(error_message)) {
        nlohmann::json error;
        error["code"] = -31;
        error["message"] = error_message;
        send("rag.error", "None", error.dump(), work_id);
        return -1;
      }
    }
    {
      std::scoped_lock lock(tasks_mutex_);
      tasks_[work_id_num] = task;
    }

    channel->subscriber_work_id(
        "", [this, weak_channel = std::weak_ptr<StackFlows::NodeChannel>(channel),
             work_id_num](const std::string& object, const std::string& data) {
          on_inference(work_id_num, weak_channel, object, data);
        });

    nlohmann::json response;
    response["service"] = "rag";
    response["status"] = "ready";
    response["backend"] = "sqlite-fts5-python-vector";
    response["model"] = task->model_path;
    response["knowledge_db"] = task->knowledge_db_path;
    send("rag.setup", response, NODE_NO_ERROR, work_id);
    return 0;
  }

  void taskinfo(const std::string& work_id, const std::string& object,
                const std::string& data) override {
    (void)object;
    (void)data;
    nlohmann::json response;
    response["service"] = "rag";
    {
      std::scoped_lock lock(tasks_mutex_);
      response["active_tasks"] = tasks_.size();
    }
    send("rag.taskinfo", response, NODE_NO_ERROR, work_id);
  }

  int exit(const std::string& work_id, const std::string& object,
           const std::string& data) override {
    (void)object;
    (void)data;
    int work_id_num = StackFlows::sample_get_work_id_num(work_id);
    if (auto channel = get_channel(work_id_num)) {
      channel->stop_subscriber_work_id("");
    }
    {
      std::scoped_lock lock(tasks_mutex_);
      tasks_.erase(work_id_num);
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
    std::unique_ptr<RagWorker> worker;

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

    RagWorker& get_worker() {
      if (!worker) {
        worker = std::make_unique<RagWorker>(script_path, model_path, knowledge_db_path,
                                             top_k, candidate_k, threshold, context_chars);
      }
      return *worker;
    }
  };

  bool run_rag(RagTask& task, const std::string& query, nlohmann::json& result,
               std::string& error_message) {
    const std::string cache_key = task.cache_key(query);
    if (task.get_cached(cache_key, result)) {
      return true;
    }

    if (!task.get_worker().request(query, result, error_message)) {
      return false;
    }
    if (result.contains("error")) {
      error_message = result.contains("error") ? result["error"].dump() : "RAG backend failed";
      return false;
    }
    if (result.contains("metrics") && result["metrics"].is_object()) {
      result["metrics"]["cache_hit"] = false;
    }
    task.put_cached(cache_key, result);
    return true;
  }

  struct InferenceRequest {
    int work_id_num = 0;
    std::weak_ptr<StackFlows::NodeChannel> weak_channel;
    std::string object;
    std::string data;
  };

  void on_inference(int work_id_num,
                    std::weak_ptr<StackFlows::NodeChannel> weak_channel,
                    const std::string& object, const std::string& data) {
    {
      std::scoped_lock lock(queue_mutex_);
      if (stopping_) {
        return;
      }
      inference_queue_.push_back({work_id_num, std::move(weak_channel), object, data});
    }
    queue_cv_.notify_one();
  }

  void worker_loop(std::stop_token stoken) {
    while (true) {
      InferenceRequest request;
      {
        std::unique_lock lock(queue_mutex_);
        queue_cv_.wait(lock, [&] {
          return stopping_ || stoken.stop_requested() || !inference_queue_.empty();
        });
        if (inference_queue_.empty()) {
          if (stopping_ || stoken.stop_requested()) {
            break;
          }
          continue;
        }
        request = std::move(inference_queue_.front());
        inference_queue_.pop_front();
      }
      process_inference(request);
    }
  }

  void process_inference(const InferenceRequest& request) {
    int work_id_num = request.work_id_num;
    const std::string& object = request.object;
    const std::string& data = request.data;
    (void)object;
    auto channel = request.weak_channel.lock();
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
    std::shared_ptr<RagTask> task;
    {
      std::scoped_lock lock(tasks_mutex_);
      auto it = tasks_.find(work_id_num);
      if (it != tasks_.end()) {
        task = it->second;
      }
    }
    nlohmann::json rag_result;
    if (task) {
      if (!task->knowledge.empty()) {
        context = task->knowledge;
      } else {
        std::string error_message;
        if (!run_rag(*task, query, rag_result, error_message)) {
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

  std::mutex tasks_mutex_;
  std::unordered_map<int, std::shared_ptr<RagTask>> tasks_;
  std::mutex queue_mutex_;
  std::condition_variable queue_cv_;
  std::deque<InferenceRequest> inference_queue_;
  std::jthread worker_;
  bool stopping_ = false;
};

}  // namespace

int main() {
  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);
  std::signal(SIGPIPE, SIG_IGN);
  mkdir("/tmp/llm", 0777);

  RagService service;
  while (!g_exit) {
    sleep(1);
  }
  return 0;
}
