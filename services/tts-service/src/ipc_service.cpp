#include <csignal>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <poll.h>
#include <signal.h>
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

bool file_exists(const std::string& path) {
  return !path.empty() && std::filesystem::exists(path);
}

bool executable_exists(const std::string& path) {
  return !path.empty() && ::access(path.c_str(), X_OK) == 0;
}

std::string first_existing_file(const std::vector<std::string>& paths) {
  for (const auto& path : paths) {
    if (file_exists(path)) {
      return path;
    }
  }
  return {};
}

std::string first_existing_executable(const std::vector<std::string>& paths) {
  for (const auto& path : paths) {
    if (executable_exists(path)) {
      return path;
    }
  }
  return {};
}

double elapsed_ms(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now() - start)
      .count();
}

struct ProcessResult {
  int exit_code = -1;
  bool timed_out = false;
  std::string message;
};

ProcessResult run_process(const std::vector<std::string>& args, int timeout_sec) {
  if (args.empty()) {
    return {-1, false, "empty command"};
  }

  pid_t pid = ::fork();
  if (pid < 0) {
    return {-1, false, std::string("fork failed: ") + std::strerror(errno)};
  }
  if (pid == 0) {
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& arg : args) {
      argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);
    ::execv(argv[0], argv.data());
    ::_exit(127);
  }

  int status = 0;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(timeout_sec);
  while (true) {
    pid_t ret = ::waitpid(pid, &status, WNOHANG);
    if (ret == pid) {
      if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        return {code, false, code == 0 ? "" : "process exited with code " + std::to_string(code)};
      }
      if (WIFSIGNALED(status)) {
        return {-1, false, "process killed by signal " + std::to_string(WTERMSIG(status))};
      }
      return {-1, false, "process ended unexpectedly"};
    }
    if (ret < 0) {
      return {-1, false, std::string("waitpid failed: ") + std::strerror(errno)};
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      ::kill(pid, SIGKILL);
      ::waitpid(pid, &status, 0);
      return {-1, true, "process timed out after " + std::to_string(timeout_sec) + "s"};
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
}

class WorkerProcess {
 public:
  WorkerProcess(std::string executable, std::string model)
      : executable_(std::move(executable)), model_(std::move(model)) {}

  ~WorkerProcess() {
    stop();
  }

  WorkerProcess(const WorkerProcess&) = delete;
  WorkerProcess& operator=(const WorkerProcess&) = delete;

  bool start(std::string* error) {
    if (running()) {
      return true;
    }

    int stdin_pipe[2] = {-1, -1};
    int stdout_pipe[2] = {-1, -1};
    if (::pipe(stdin_pipe) != 0 || ::pipe(stdout_pipe) != 0) {
      if (error) {
        *error = std::string("pipe failed: ") + std::strerror(errno);
      }
      close_pipe(stdin_pipe);
      close_pipe(stdout_pipe);
      return false;
    }

    pid_ = ::fork();
    if (pid_ < 0) {
      if (error) {
        *error = std::string("fork failed: ") + std::strerror(errno);
      }
      close_pipe(stdin_pipe);
      close_pipe(stdout_pipe);
      pid_ = -1;
      return false;
    }

    if (pid_ == 0) {
      ::dup2(stdin_pipe[0], STDIN_FILENO);
      ::dup2(stdout_pipe[1], STDOUT_FILENO);
      ::close(stdin_pipe[0]);
      ::close(stdin_pipe[1]);
      ::close(stdout_pipe[0]);
      ::close(stdout_pipe[1]);
      std::vector<char*> argv;
      argv.push_back(const_cast<char*>(executable_.c_str()));
      argv.push_back(const_cast<char*>("--model"));
      argv.push_back(const_cast<char*>(model_.c_str()));
      argv.push_back(nullptr);
      ::execv(argv[0], argv.data());
      ::_exit(127);
    }

    ::close(stdin_pipe[0]);
    ::close(stdout_pipe[1]);
    stdin_fd_ = stdin_pipe[1];
    stdout_fd_ = stdout_pipe[0];
    return true;
  }

  bool request(const nlohmann::json& payload, int timeout_sec,
               nlohmann::json* response, std::string* error) {
    std::scoped_lock lock(io_mutex_);
    if (!running()) {
      if (error) {
        *error = "worker is not running";
      }
      return false;
    }

    std::string raw = payload.dump();
    raw.push_back('\n');
    if (!write_all(raw)) {
      if (error) {
        *error = std::string("worker write failed: ") + std::strerror(errno);
      }
      stop_locked();
      return false;
    }

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(timeout_sec);
    std::string line;
    while (true) {
      if (!read_line(deadline, &line, error)) {
        stop_locked();
        return false;
      }
      try {
        *response = nlohmann::json::parse(line);
        return true;
      } catch (...) {
        // SummerTTS dependencies may write banners to stdout. Skip non-JSON lines.
      }
    }
  }

  void stop() {
    std::scoped_lock lock(io_mutex_);
    stop_locked();
  }

 private:
  static void close_pipe(int pipefd[2]) {
    if (pipefd[0] >= 0) {
      ::close(pipefd[0]);
    }
    if (pipefd[1] >= 0) {
      ::close(pipefd[1]);
    }
  }

  bool running() const {
    return pid_ > 0 && stdin_fd_ >= 0 && stdout_fd_ >= 0;
  }

  bool write_all(const std::string& raw) {
    const char* data = raw.data();
    size_t left = raw.size();
    while (left > 0) {
      ssize_t written = ::write(stdin_fd_, data, left);
      if (written < 0) {
        if (errno == EINTR) {
          continue;
        }
        return false;
      }
      data += written;
      left -= static_cast<size_t>(written);
    }
    return true;
  }

  bool read_line(std::chrono::steady_clock::time_point deadline,
                 std::string* line, std::string* error) {
    line->clear();
    while (true) {
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) {
        if (error) {
          *error = "worker timed out";
        }
        return false;
      }
      const auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    deadline - now)
                                    .count();
      struct pollfd pfd;
      pfd.fd = stdout_fd_;
      pfd.events = POLLIN | POLLHUP | POLLERR;
      pfd.revents = 0;
      const int ret = ::poll(&pfd, 1, static_cast<int>(remaining_ms));
      if (ret < 0) {
        if (errno == EINTR) {
          continue;
        }
        if (error) {
          *error = std::string("poll failed: ") + std::strerror(errno);
        }
        return false;
      }
      if (ret == 0) {
        if (error) {
          *error = "worker timed out";
        }
        return false;
      }
      if (pfd.revents & POLLIN) {
        char ch = '\0';
        ssize_t n = ::read(stdout_fd_, &ch, 1);
        if (n < 0) {
          if (errno == EINTR) {
            continue;
          }
          if (error) {
            *error = std::string("read failed: ") + std::strerror(errno);
          }
          return false;
        }
        if (n == 0) {
          if (error) {
            *error = "worker closed stdout";
          }
          return false;
        }
        if (ch == '\n') {
          return true;
        }
        line->push_back(ch);
        continue;
      }
      if (pfd.revents & (POLLHUP | POLLERR)) {
        if (error) {
          *error = "worker closed stdout";
        }
        return false;
      }
    }
  }

  void stop_locked() {
    if (stdin_fd_ >= 0) {
      const std::string quit = "{\"quit\":true}\n";
      (void)::write(stdin_fd_, quit.data(), quit.size());
      ::close(stdin_fd_);
      stdin_fd_ = -1;
    }
    if (stdout_fd_ >= 0) {
      ::close(stdout_fd_);
      stdout_fd_ = -1;
    }
    if (pid_ > 0) {
      int status = 0;
      for (int i = 0; i < 10; ++i) {
        pid_t ret = ::waitpid(pid_, &status, WNOHANG);
        if (ret == pid_) {
          pid_ = -1;
          return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
      ::kill(pid_, SIGTERM);
      for (int i = 0; i < 10; ++i) {
        pid_t ret = ::waitpid(pid_, &status, WNOHANG);
        if (ret == pid_) {
          pid_ = -1;
          return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
      ::kill(pid_, SIGKILL);
      ::waitpid(pid_, &status, 0);
      pid_ = -1;
    }
  }

  std::string executable_;
  std::string model_;
  pid_t pid_ = -1;
  int stdin_fd_ = -1;
  int stdout_fd_ = -1;
  std::mutex io_mutex_;
};

class TtsIpcService : public StackFlows::StackFlow {
 public:
  TtsIpcService() : StackFlow("tts") {}

  int setup(const std::string& work_id, const std::string& object,
            const std::string& data) override {
    (void)object;
    int work_id_num = StackFlows::sample_get_work_id_num(work_id);
    auto channel = get_channel(work_id);
    if (!channel) {
      send("tts.error", "missing channel", NODE_NO_ERROR, work_id);
      return -1;
    }
    channel->set_output(true);
    channel->set_stream(false);

    TtsTask task;
    task.edge_tts_executable = default_edge_tts_executable();
    task.edge_tts_worker_executable = default_edge_tts_worker_executable();
    task.model = default_tts_model();
    try {
      auto config = nlohmann::json::parse(data.empty() ? "{}" : data);
      if (config.contains("output_dir") && config["output_dir"].is_string()) {
        task.output_dir = config["output_dir"].get<std::string>();
      }
      if (config.contains("model") && config["model"].is_string()) {
        task.model = config["model"].get<std::string>();
      }
      if (config.contains("edge_tts_executable") && config["edge_tts_executable"].is_string()) {
        task.edge_tts_executable = config["edge_tts_executable"].get<std::string>();
      }
      if (config.contains("edge_tts_worker_executable") &&
          config["edge_tts_worker_executable"].is_string()) {
        task.edge_tts_worker_executable =
            config["edge_tts_worker_executable"].get<std::string>();
      }
      if (config.contains("backend") && config["backend"].is_string()) {
        task.requested_backend = config["backend"].get<std::string>();
      }
      if (config.contains("stream_segments") && config["stream_segments"].is_boolean()) {
        task.stream_segments = config["stream_segments"].get<bool>();
      }
      if (config.contains("speaker_id") && config["speaker_id"].is_number_integer()) {
        task.speaker_id = config["speaker_id"].get<int>();
      }
      if (config.contains("length_scale") && config["length_scale"].is_number()) {
        task.length_scale = config["length_scale"].get<float>();
      }
      if (config.contains("require_real_backend") && config["require_real_backend"].is_boolean()) {
        task.require_real_backend = config["require_real_backend"].get<bool>();
      }
      if (config.contains("timeout_sec") && config["timeout_sec"].is_number_integer()) {
        task.timeout_sec = config["timeout_sec"].get<int>();
      }
    } catch (...) {
    }
    if (task.timeout_sec <= 0) {
      task.timeout_sec = 120;
    }
    std::filesystem::create_directories(task.output_dir);
    const bool worker_available =
        executable_exists(task.edge_tts_worker_executable) && file_exists(task.model);
    const bool subprocess_available =
        executable_exists(task.edge_tts_executable) && file_exists(task.model);

    if (worker_available && task.requested_backend != "summertts-subprocess") {
      task.worker = std::make_shared<WorkerProcess>(task.edge_tts_worker_executable, task.model);
      std::string worker_error;
      if (task.worker->start(&worker_error)) {
        task.backend = "summertts-worker";
        task.real_backend_available = true;
      } else {
        task.worker.reset();
        task.worker_start_error = worker_error;
      }
    }
    const bool worker_required = task.requested_backend == "summertts-worker";
    if (!task.real_backend_available && subprocess_available && !worker_required) {
      task.backend = "summertts-subprocess";
      task.real_backend_available = true;
    }
    if (!task.real_backend_available) {
      task.backend = "phase1-text-artifact";
    }
    if (task.require_real_backend && !task.real_backend_available) {
      nlohmann::json error;
      error["code"] = -61;
      error["message"] = task.worker_start_error.empty()
          ? "SummerTTS worker/subprocess executable or model not found"
          : task.worker_start_error;
      error["edge_tts_worker_executable"] = task.edge_tts_worker_executable;
      error["edge_tts_executable"] = task.edge_tts_executable;
      error["model"] = task.model;
      send("tts.error", "None", error.dump(), work_id);
      return -1;
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
    response["service"] = "tts";
    response["status"] = "ready";
    response["backend"] = task.backend;
    response["edge_tts_worker_executable"] = task.edge_tts_worker_executable;
    response["edge_tts_executable"] = task.edge_tts_executable;
    response["model"] = task.model;
    response["output_dir"] = task.output_dir;
    response["speaker_id"] = task.speaker_id;
    response["length_scale"] = task.length_scale;
    response["stream_segments"] = task.stream_segments;
    send("tts.setup", response, NODE_NO_ERROR, work_id);
    return 0;
  }

  void taskinfo(const std::string& work_id, const std::string& object,
                const std::string& data) override {
    (void)object;
    (void)data;
    nlohmann::json response;
    response["service"] = "tts";
    {
      std::scoped_lock lock(tasks_mutex_);
      response["active_tasks"] = tasks_.size();
    }
    send("tts.taskinfo", response, NODE_NO_ERROR, work_id);
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
    send("tts.exit", "ok", NODE_NO_ERROR, work_id);
    return 0;
  }

 private:
  struct TtsTask {
    std::string output_dir = "/tmp/edge_voice_tts";
    std::string model;
    std::string edge_tts_executable;
    std::string edge_tts_worker_executable;
    std::string requested_backend;
    std::string worker_start_error;
    std::string backend = "phase1-text-artifact";
    int speaker_id = 0;
    float length_scale = 1.0f;
    int timeout_sec = 120;
    bool require_real_backend = false;
    bool real_backend_available = false;
    bool stream_segments = false;
    int counter = 0;
    std::shared_ptr<WorkerProcess> worker;
  };

  std::string default_edge_tts_worker_executable() const {
    return first_existing_executable({
        "build-phase1/services/tts-service/edge_tts_worker",
        "build-tts/services/tts-service/edge_tts_worker",
        "services/tts-service/build/edge_tts_worker",
        "build/services/tts-service/edge_tts_worker",
    });
  }

  std::string default_edge_tts_executable() const {
    return first_existing_executable({
        "build-phase1/services/tts-service/edge_tts_service",
        "build-tts/services/tts-service/edge_tts_service",
        "services/tts-service/build/edge_tts_service",
        "build/services/tts-service/edge_tts_service",
    });
  }

  std::string default_tts_model() const {
    return first_existing_file({
        "third-party/SummerTTS/models/single_speaker_fast.bin",
        "third-party/SummerTTS/model/single_speaker_fast.bin",
        "third-party/SummerTTS/resource/single_speaker_fast.bin",
        "third-party/SummerTTS/single_speaker_fast.bin",
    });
  }

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

    TtsTask task;
    {
      std::scoped_lock lock(tasks_mutex_);
      auto it = tasks_.find(work_id_num);
      if (it == tasks_.end()) {
        nlohmann::json error;
        error["code"] = -6;
        error["message"] = "Unit does not exist";
        channel->send("tts.error", "None", error.dump());
        return;
      }
      task = it->second;
      task.counter++;
      it->second.counter = task.counter;
    }
    std::filesystem::create_directories(task.output_dir);
    const auto start = std::chrono::steady_clock::now();

    if (task.backend == "summertts-worker" && task.worker) {
      const std::string artifact = task.output_dir + "/tts_" +
          std::to_string(work_id_num) + "_" + std::to_string(task.counter) + ".wav";
      nlohmann::json request;
      request["request_id"] = "tts-" + std::to_string(work_id_num) + "-" +
          std::to_string(task.counter);
      request["text"] = text;
      request["output"] = artifact;
      request["speaker_id"] = task.speaker_id;
      request["length_scale"] = task.length_scale;
      request["segment_index"] = task.counter - 1;

      nlohmann::json worker_response;
      std::string worker_error;
      if (!task.worker->request(request, task.timeout_sec, &worker_response, &worker_error) ||
          !worker_response.value("ok", false) || !file_exists(artifact) ||
          std::filesystem::file_size(artifact) <= 44) {
        nlohmann::json error;
        error["code"] = -63;
        if (!worker_error.empty()) {
          error["message"] = worker_error;
        } else if (worker_response.contains("error")) {
          error["message"] = worker_response["error"].dump();
        } else {
          error["message"] = "SummerTTS worker failed";
        }
        error["artifact"] = artifact;
        error["edge_tts_worker_executable"] = task.edge_tts_worker_executable;
        error["model"] = task.model;
        error["metrics"]["total_ms"] = elapsed_ms(start);
        channel->send("tts.error", "None", error.dump());
        return;
      }

      nlohmann::json response;
      response["text"] = text;
      response["artifact"] = artifact;
      response["mime_type"] = "audio/wav";
      response["backend"] = task.backend;
      response["sample_rate"] = worker_response.value("sample_rate", 16000);
      response["speaker_id"] = worker_response.value("speaker_id", task.speaker_id);
      response["segment_index"] = task.counter - 1;
      response["model"] = task.model;
      response["metrics"]["total_ms"] = elapsed_ms(start);
      response["metrics"]["worker_ms"] =
          worker_response.value("metrics", nlohmann::json::object()).value("worker_ms", 0.0);
      channel->send("tts.response", response, NODE_NO_ERROR);
      return;
    }

    if (task.backend == "summertts-subprocess") {
      const std::string artifact = task.output_dir + "/tts_" +
          std::to_string(work_id_num) + "_" + std::to_string(task.counter) + ".wav";
      std::vector<std::string> args = {
          task.edge_tts_executable,
          "--model", task.model,
          "--text", text,
          "--output", artifact,
          "--speaker-id", std::to_string(task.speaker_id),
          "--length-scale", std::to_string(task.length_scale),
      };
      ProcessResult result = run_process(args, task.timeout_sec);
      if (result.exit_code != 0 || result.timed_out || !file_exists(artifact) ||
          std::filesystem::file_size(artifact) <= 44) {
        nlohmann::json error;
        error["code"] = -62;
        error["message"] = result.message.empty() ? "SummerTTS failed" : result.message;
        error["artifact"] = artifact;
        error["edge_tts_executable"] = task.edge_tts_executable;
        error["model"] = task.model;
        error["metrics"]["total_ms"] = elapsed_ms(start);
        channel->send("tts.error", "None", error.dump());
        return;
      }

      nlohmann::json response;
      response["text"] = text;
      response["artifact"] = artifact;
      response["mime_type"] = "audio/wav";
      response["backend"] = task.backend;
      response["sample_rate"] = 16000;
      response["speaker_id"] = task.speaker_id;
      response["segment_index"] = task.counter - 1;
      response["model"] = task.model;
      response["metrics"]["total_ms"] = elapsed_ms(start);
      channel->send("tts.response", response, NODE_NO_ERROR);
      return;
    }

    const std::string artifact = task.output_dir + "/tts_phase1_" +
        std::to_string(work_id_num) + "_" + std::to_string(task.counter) + ".txt";
    std::ofstream out(artifact);
    out << text << "\n";
    out.close();

    nlohmann::json response;
    response["text"] = text;
    response["artifact"] = artifact;
    response["mime_type"] = "text/plain";
    response["backend"] = task.backend;
    response["segment_index"] = task.counter - 1;
    response["metrics"]["total_ms"] = elapsed_ms(start);
    channel->send("tts.response", response, NODE_NO_ERROR);
  }

  std::mutex tasks_mutex_;
  std::unordered_map<int, TtsTask> tasks_;
};

}  // namespace

int main() {
  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);
  std::signal(SIGPIPE, SIG_IGN);
  mkdir("/tmp/llm", 0777);

  TtsIpcService service;
  while (!g_exit) {
    sleep(1);
  }
  return 0;
}
