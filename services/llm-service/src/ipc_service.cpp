#include <csignal>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "StackFlow.h"
#include "json.hpp"

#if EDGE_ENABLE_RKLLM
#include "rkllm.h"
#endif

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

std::string first_existing_path(const std::vector<std::string>& candidates) {
  for (const auto& path : candidates) {
    if (access(path.c_str(), F_OK) == 0) {
      return path;
    }
  }
  return candidates.empty() ? std::string() : candidates.front();
}

#if EDGE_ENABLE_RKLLM
std::mutex g_rkllm_run_mutex;

using Clock = std::chrono::steady_clock;

struct RkllmRunState {
  std::string* output = nullptr;
  std::function<void(const std::string&, size_t, double)> on_delta;
  Clock::time_point start;
  bool saw_first_token = false;
  double first_token_ms = -1.0;
  size_t delta_count = 0;
};

RkllmRunState* g_rkllm_state = nullptr;

double elapsed_ms(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

int rkllm_callback(RKLLMResult* result, void* userdata, LLMCallState state) {
  (void)userdata;
  if (state == RKLLM_RUN_NORMAL) {
    if (result != nullptr && result->text != nullptr && g_rkllm_state != nullptr &&
        g_rkllm_state->output != nullptr) {
      std::string delta = result->text;
      if (delta.empty()) {
        return 0;
      }
      g_rkllm_state->output->append(delta);
      ++g_rkllm_state->delta_count;
      if (!g_rkllm_state->saw_first_token) {
        g_rkllm_state->saw_first_token = true;
        g_rkllm_state->first_token_ms = elapsed_ms(g_rkllm_state->start);
      }
      if (g_rkllm_state->on_delta) {
        g_rkllm_state->on_delta(delta, g_rkllm_state->delta_count,
                                g_rkllm_state->first_token_ms);
      }
    }
  }
  return 0;
}

class RkllmRuntime {
 public:
  ~RkllmRuntime() {
    if (handle_ != nullptr) {
      rkllm_destroy(handle_);
      handle_ = nullptr;
    }
  }

  bool init(const std::string& model_path, int max_new_tokens, int max_context_len,
            std::string& error_message) {
    if (handle_ != nullptr) {
      return true;
    }
    RKLLMParam param = rkllm_createDefaultParam();
    param.model_path = const_cast<char*>(model_path.c_str());
    param.top_k = 1;
    param.top_p = 0.95f;
    param.temperature = 0.8f;
    param.repeat_penalty = 1.1f;
    param.frequency_penalty = 0.0f;
    param.presence_penalty = 0.0f;
    param.max_new_tokens = max_new_tokens;
    param.max_context_len = max_context_len;
    param.skip_special_token = true;
    param.extend_param.base_domain_id = 0;
    param.extend_param.embed_flash = 1;

    int ret = rkllm_init(&handle_, &param, rkllm_callback);
    if (ret != 0) {
      error_message = "rkllm_init failed, ret=" + std::to_string(ret);
      handle_ = nullptr;
      return false;
    }
    return true;
  }

  bool generate(const std::string& prompt, std::string& text, nlohmann::json& metrics,
                const std::function<void(const std::string&, size_t, double)>& on_delta,
                std::string& error_message) {
    if (handle_ == nullptr) {
      error_message = "RKLLM handle is not initialized";
      return false;
    }
    std::lock_guard<std::mutex> lock(g_rkllm_run_mutex);
    text.clear();
    RkllmRunState state;
    state.output = &text;
    state.on_delta = on_delta;
    state.start = Clock::now();
    g_rkllm_state = &state;

    RKLLMInput input;
    std::memset(&input, 0, sizeof(input));
    RKLLMInferParam infer_param;
    std::memset(&infer_param, 0, sizeof(infer_param));
    infer_param.mode = RKLLM_INFER_GENERATE;
    infer_param.keep_history = 0;
    input.input_type = RKLLM_INPUT_PROMPT;
    input.role = const_cast<char*>("user");
    input.prompt_input = const_cast<char*>(prompt.c_str());

    int ret = rkllm_run(handle_, &input, &infer_param, nullptr);
    const double total_ms = elapsed_ms(state.start);
    g_rkllm_state = nullptr;
    if (ret != 0) {
      error_message = "rkllm_run failed, ret=" + std::to_string(ret);
      return false;
    }
    metrics["first_token_ms"] = state.first_token_ms;
    metrics["total_ms"] = total_ms;
    metrics["delta_count"] = state.delta_count;
    metrics["stream"] = true;
    return true;
  }

 private:
  LLMHandle handle_ = nullptr;
};
#endif

class LlmIpcService : public StackFlows::StackFlow {
 public:
  LlmIpcService() : StackFlow("llm") {
    worker_ = std::jthread([this](std::stop_token stoken) { worker_loop(stoken); });
  }

  ~LlmIpcService() override {
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
      send("llm.error", "missing channel", NODE_NO_ERROR, work_id);
      return -1;
    }
    channel->set_output(true);
    channel->set_stream(true);

    auto task = std::make_shared<LlmTask>();
    try {
      auto config = nlohmann::json::parse(data.empty() ? "{}" : data);
      if (config.contains("model") && config["model"].is_string()) {
        task->model = config["model"].get<std::string>();
      }
      if (config.contains("max_new_tokens") && config["max_new_tokens"].is_number_integer()) {
        task->max_new_tokens = config["max_new_tokens"].get<int>();
      }
      if (config.contains("max_context_len") && config["max_context_len"].is_number_integer()) {
        task->max_context_len = config["max_context_len"].get<int>();
      }
      if (config.contains("require_real_backend") && config["require_real_backend"].is_boolean()) {
        task->require_real_backend = config["require_real_backend"].get<bool>();
      }
    } catch (...) {
    }
    task->fill_defaults();

#if EDGE_ENABLE_RKLLM
    task->backend = "rkllm";
    task->runtime = std::make_unique<RkllmRuntime>();
    std::string error_message;
    if (!task->runtime->init(task->model, task->max_new_tokens, task->max_context_len, error_message)) {
      nlohmann::json error;
      error["code"] = -51;
      error["message"] = error_message;
      send("llm.error", "None", error.dump(), work_id);
      return -1;
    }
#else
    task->backend = "mock-no-rkllm-runtime";
    if (task->require_real_backend) {
      nlohmann::json error;
      error["code"] = -52;
      error["message"] = "RKLLM runtime was not found at build time";
      send("llm.error", "None", error.dump(), work_id);
      return -1;
    }
#endif

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
    response["service"] = "llm";
    response["status"] = "ready";
    response["backend"] = task->backend;
    response["model"] = task->model;
    send("llm.setup", response, NODE_NO_ERROR, work_id);
    return 0;
  }

  void taskinfo(const std::string& work_id, const std::string& object,
                const std::string& data) override {
    (void)object;
    (void)data;
    nlohmann::json response;
    response["service"] = "llm";
    {
      std::scoped_lock lock(tasks_mutex_);
      response["active_tasks"] = tasks_.size();
    }
    send("llm.taskinfo", response, NODE_NO_ERROR, work_id);
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
    send("llm.exit", "ok", NODE_NO_ERROR, work_id);
    return 0;
  }

 private:
  struct LlmTask {
    std::string model;
    std::string backend = "mock-no-rkllm-runtime";
    int max_new_tokens = 512;
    int max_context_len = 4096;
    bool require_real_backend = false;
#if EDGE_ENABLE_RKLLM
    std::unique_ptr<RkllmRuntime> runtime;
#endif

    void fill_defaults() {
      if (model.empty() || model == "phase1-mock") {
        model = first_existing_path({
            "models/llm/Qwen3-1.7B.rkllm",
            "../models/llm/Qwen3-1.7B.rkllm",
            "/home/pi/Edge-Voice-Infra/models/llm/Qwen3-1.7B.rkllm",
        });
      }
    }
  };

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
    const std::string prompt = extract_prompt(data);
    if (prompt.empty()) {
      nlohmann::json error;
      error["code"] = -24;
      error["message"] = "empty prompt";
      channel->send("llm.error", "None", error.dump());
      return;
    }

    std::shared_ptr<LlmTask> task;
    {
      std::scoped_lock lock(tasks_mutex_);
      auto it = tasks_.find(work_id_num);
      if (it != tasks_.end()) {
        task = it->second;
      }
    }
    if (!task) {
      nlohmann::json error;
      error["code"] = -6;
      error["message"] = "Unit does not exist";
      channel->send("llm.error", "None", error.dump());
      return;
    }

    std::string text;
    std::string error_message;
    nlohmann::json metrics;
#if EDGE_ENABLE_RKLLM
    if (task->runtime) {
      auto delta_sender = [channel](const std::string& delta, size_t index, double first_token_ms) {
        nlohmann::json event;
        event["delta"] = delta;
        event["index"] = index;
        event["finish"] = false;
        event["first_token_ms"] = first_token_ms;
        channel->send("llm.delta", event, NODE_NO_ERROR);
      };
      if (!task->runtime->generate(prompt, text, metrics, delta_sender, error_message)) {
        nlohmann::json error;
        error["code"] = -53;
        error["message"] = error_message;
        channel->send("llm.error", "None", error.dump());
        return;
      }
    } else
#endif
    {
      text = "Mock LLM response: " + prompt;
      metrics["first_token_ms"] = 0.0;
      metrics["total_ms"] = 0.0;
      metrics["delta_count"] = 1;
      metrics["stream"] = true;
      nlohmann::json event;
      event["delta"] = text;
      event["index"] = 1;
      event["finish"] = false;
      event["first_token_ms"] = 0.0;
      channel->send("llm.delta", event, NODE_NO_ERROR);
    }

    nlohmann::json response;
    response["model"] = task->model;
    response["backend"] = task->backend;
    response["text"] = text;
    response["finish"] = true;
    response["metrics"] = metrics;
    channel->send("llm.response", response, NODE_NO_ERROR);
  }

  std::mutex tasks_mutex_;
  std::unordered_map<int, std::shared_ptr<LlmTask>> tasks_;
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
  mkdir("/tmp/llm", 0777);

  LlmIpcService service;
  while (!g_exit) {
    sleep(1);
  }
  return 0;
}
