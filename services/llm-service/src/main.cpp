#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

#include "rkllm.h"

namespace {

LLMHandle g_handle = nullptr;
std::string g_stream_text;

void destroy_llm() {
  if (g_handle != nullptr) {
    LLMHandle tmp = g_handle;
    g_handle = nullptr;
    rkllm_destroy(tmp);
  }
}

void signal_handler(int sig) {
  std::cerr << "\n[llm-service] caught signal " << sig << ", cleaning up..." << std::endl;
  destroy_llm();
  std::_Exit(sig);
}

int llm_callback(RKLLMResult *result, void *userdata, LLMCallState state) {
  (void)userdata;
  if (state == RKLLM_RUN_NORMAL) {
    if (result != nullptr && result->text != nullptr) {
      std::cout << result->text << std::flush;
      g_stream_text += result->text;
    }
  } else if (state == RKLLM_RUN_FINISH) {
    std::cout << std::endl;
  } else if (state == RKLLM_RUN_ERROR) {
    std::cerr << "\n[llm-service] run error" << std::endl;
  }
  return 0;
}

bool file_exists(const std::string &path) {
  return access(path.c_str(), F_OK) == 0;
}

std::string find_default_model() {
  const std::vector<std::string> candidates = {
      "models/llm/Qwen3-1.7B.rkllm",
      "../models/llm/Qwen3-1.7B.rkllm",
      "../../models/llm/Qwen3-1.7B.rkllm",
      "/home/pi/Edge-Voice-Infra/models/llm/Qwen3-1.7B.rkllm"};
  for (const auto &path : candidates) {
    if (file_exists(path)) {
      return path;
    }
  }
  return "";
}

void print_usage(const char *prog) {
  std::cout << "Usage: " << prog
            << " [--model path] [--max-new-tokens N] [--max-context-len N]\n"
               "Examples:\n"
               "  "
            << prog
            << " --model models/llm/Qwen3-1.7B.rkllm --max-new-tokens 512 --max-context-len 4096\n";
}

}  // namespace

int main(int argc, char **argv) {
  std::string model_path = find_default_model();
  int max_new_tokens = 512;
  int max_context_len = 4096;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--model" && i + 1 < argc) {
      model_path = argv[++i];
    } else if (arg == "--max-new-tokens" && i + 1 < argc) {
      max_new_tokens = std::atoi(argv[++i]);
    } else if (arg == "--max-context-len" && i + 1 < argc) {
      max_context_len = std::atoi(argv[++i]);
    } else if (arg == "--help" || arg == "-h") {
      print_usage(argv[0]);
      return 0;
    } else {
      std::cerr << "Unknown arg: " << arg << std::endl;
      print_usage(argv[0]);
      return 1;
    }
  }

  if (model_path.empty() || !file_exists(model_path)) {
    std::cerr << "[llm-service] model not found. Please pass --model, e.g.\n"
                 "  --model /home/pi/Edge-Voice-Infra/models/llm/Qwen3-1.7B.rkllm"
              << std::endl;
    return 1;
  }

  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);

  std::cout << "[llm-service] init model: " << model_path << std::endl;

  RKLLMParam param = rkllm_createDefaultParam();
  param.model_path = const_cast<char *>(model_path.c_str());
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

  if (rkllm_init(&g_handle, &param, llm_callback) != 0) {
    std::cerr << "[llm-service] rkllm_init failed" << std::endl;
    destroy_llm();
    return 1;
  }
  std::cout << "[llm-service] rkllm_init success" << std::endl;

  RKLLMInput input;
  std::memset(&input, 0, sizeof(input));
  RKLLMInferParam infer_param;
  std::memset(&infer_param, 0, sizeof(infer_param));
  infer_param.mode = RKLLM_INFER_GENERATE;
  infer_param.keep_history = 0;

  std::cout << "Type your prompt (exit to quit, clear to clear kv cache)\n";
  while (true) {
    std::string prompt;
    std::cout << "\nuser> " << std::flush;
    if (!std::getline(std::cin, prompt)) {
      break;
    }

    if (prompt == "exit") {
      break;
    }
    if (prompt == "clear") {
      const int ret = rkllm_clear_kv_cache(g_handle, 1, nullptr, nullptr);
      if (ret != 0) {
        std::cerr << "[llm-service] clear kv cache failed, ret=" << ret << std::endl;
      } else {
        std::cout << "[llm-service] kv cache cleared" << std::endl;
      }
      continue;
    }

    g_stream_text.clear();
    input.input_type = RKLLM_INPUT_PROMPT;
    input.role = const_cast<char *>("user");
    input.prompt_input = const_cast<char *>(prompt.c_str());

    std::cout << "assistant> " << std::flush;
    const int ret = rkllm_run(g_handle, &input, &infer_param, nullptr);
    if (ret != 0) {
      std::cerr << "\n[llm-service] rkllm_run failed, ret=" << ret << std::endl;
    }
  }

  destroy_llm();
  std::cout << "[llm-service] bye" << std::endl;
  return 0;
}
