#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "SynthesizerTrn.h"
#include "json.hpp"
#include "utils.h"

namespace {

struct WorkerConfig {
  std::string model_path;
};

double elapsed_ms(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now() - start)
      .count();
}

bool file_exists(const std::string& path) {
  std::ifstream ifs(path, std::ios::binary);
  return ifs.good();
}

void print_usage(const char* prog) {
  std::cerr << "Usage: " << prog << " --model <model.bin>\n";
}

bool parse_args(int argc, char** argv, WorkerConfig* cfg) {
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--model" && i + 1 < argc) {
      cfg->model_path = argv[++i];
    } else if (arg == "--help" || arg == "-h") {
      print_usage(argv[0]);
      return false;
    } else {
      std::cerr << "[tts-worker] unknown arg: " << arg << "\n";
      print_usage(argv[0]);
      return false;
    }
  }
  if (cfg->model_path.empty()) {
    std::cerr << "[tts-worker] --model is required\n";
    return false;
  }
  return true;
}

std::vector<char> build_wav_buffer(const int16_t* pcm, int sample_count) {
  constexpr int kSampleRate = 16000;
  constexpr int kChannels = 1;
  constexpr int kBitsPerSample = 16;
  constexpr int kWavHeaderSize = 44;

  const int pcm_bytes = sample_count * static_cast<int>(sizeof(int16_t));
  const int total_data_len = pcm_bytes + 36;
  const int byte_rate = kSampleRate * kChannels * kBitsPerSample / 8;

  std::vector<char> wav(kWavHeaderSize + pcm_bytes, 0);
  char* header = wav.data();
  header[0] = 'R';
  header[1] = 'I';
  header[2] = 'F';
  header[3] = 'F';
  header[4] = static_cast<char>(total_data_len & 0xff);
  header[5] = static_cast<char>((total_data_len >> 8) & 0xff);
  header[6] = static_cast<char>((total_data_len >> 16) & 0xff);
  header[7] = static_cast<char>((total_data_len >> 24) & 0xff);
  header[8] = 'W';
  header[9] = 'A';
  header[10] = 'V';
  header[11] = 'E';
  header[12] = 'f';
  header[13] = 'm';
  header[14] = 't';
  header[15] = ' ';
  header[16] = 16;
  header[20] = 1;
  header[22] = static_cast<char>(kChannels);
  header[24] = static_cast<char>(kSampleRate & 0xff);
  header[25] = static_cast<char>((kSampleRate >> 8) & 0xff);
  header[26] = static_cast<char>((kSampleRate >> 16) & 0xff);
  header[27] = static_cast<char>((kSampleRate >> 24) & 0xff);
  header[28] = static_cast<char>(byte_rate & 0xff);
  header[29] = static_cast<char>((byte_rate >> 8) & 0xff);
  header[30] = static_cast<char>((byte_rate >> 16) & 0xff);
  header[31] = static_cast<char>((byte_rate >> 24) & 0xff);
  header[32] = static_cast<char>(kChannels * kBitsPerSample / 8);
  header[34] = static_cast<char>(kBitsPerSample);
  header[36] = 'd';
  header[37] = 'a';
  header[38] = 't';
  header[39] = 'a';
  header[40] = static_cast<char>(pcm_bytes & 0xff);
  header[41] = static_cast<char>((pcm_bytes >> 8) & 0xff);
  header[42] = static_cast<char>((pcm_bytes >> 16) & 0xff);
  header[43] = static_cast<char>((pcm_bytes >> 24) & 0xff);

  std::memcpy(wav.data() + kWavHeaderSize, pcm, pcm_bytes);
  return wav;
}

class TtsWorker {
 public:
  explicit TtsWorker(const std::string& model_path) {
    if (!file_exists(model_path)) {
      throw std::runtime_error("model not found: " + model_path);
    }
    model_size_ = ttsLoadModel(const_cast<char*>(model_path.c_str()), &model_data_);
    if (model_size_ <= 0 || model_data_ == nullptr) {
      throw std::runtime_error("failed to load model: " + model_path);
    }
    synthesizer_.reset(new SynthesizerTrn(model_data_, model_size_));
    speaker_num_ = synthesizer_->getSpeakerNum();
    std::cerr << "[tts-worker] model loaded, speakers=" << speaker_num_ << "\n";
  }

  ~TtsWorker() {
    if (model_data_ != nullptr) {
      tts_free_data(model_data_);
    }
  }

  nlohmann::json synthesize(const nlohmann::json& request) {
    const auto start = std::chrono::steady_clock::now();
    nlohmann::json response;
    try {
      const std::string text = request.value("text", "");
      const std::string output = request.value("output", "");
      int speaker_id = request.value("speaker_id", 0);
      const float length_scale = request.value("length_scale", 1.0f);
      response["request_id"] = request.value("request_id", "");
      response["segment_index"] = request.value("segment_index", -1);

      if (text.empty()) {
        throw std::runtime_error("empty text");
      }
      if (output.empty()) {
        throw std::runtime_error("empty output path");
      }
      if (speaker_id < 0 || speaker_id >= speaker_num_) {
        speaker_id = 0;
      }

      int32_t sample_count = 0;
      int16_t* pcm = synthesizer_->infer(text, speaker_id, length_scale, sample_count);
      if (pcm == nullptr || sample_count <= 0) {
        throw std::runtime_error("synthesize failed");
      }

      std::vector<char> wav = build_wav_buffer(pcm, sample_count);
      std::ofstream ofs(output, std::ios::binary);
      if (!ofs.is_open()) {
        tts_free_data(pcm);
        throw std::runtime_error("failed to open output: " + output);
      }
      ofs.write(wav.data(), static_cast<std::streamsize>(wav.size()));
      ofs.close();
      tts_free_data(pcm);

      response["ok"] = true;
      response["artifact"] = output;
      response["mime_type"] = "audio/wav";
      response["sample_rate"] = 16000;
      response["speaker_id"] = speaker_id;
      response["metrics"]["worker_ms"] = elapsed_ms(start);
      response["metrics"]["sample_count"] = sample_count;
    } catch (const std::exception& e) {
      response["ok"] = false;
      response["error"]["code"] = -1;
      response["error"]["message"] = e.what();
      response["metrics"]["worker_ms"] = elapsed_ms(start);
    }
    return response;
  }

 private:
  float* model_data_ = nullptr;
  int32_t model_size_ = 0;
  int speaker_num_ = 0;
  std::unique_ptr<SynthesizerTrn> synthesizer_;
};

}  // namespace

int main(int argc, char** argv) {
  WorkerConfig cfg;
  if (!parse_args(argc, argv, &cfg)) {
    return 1;
  }

  try {
    TtsWorker worker(cfg.model_path);
    std::string line;
    while (std::getline(std::cin, line)) {
      if (line.empty()) {
        continue;
      }
      nlohmann::json response;
      try {
        auto request = nlohmann::json::parse(line);
        if (request.value("quit", false)) {
          break;
        }
        response = worker.synthesize(request);
      } catch (const std::exception& e) {
        response["ok"] = false;
        response["error"]["code"] = -2;
        response["error"]["message"] = e.what();
      }
      std::cout << response.dump() << std::endl;
    }
  } catch (const std::exception& e) {
    std::cerr << "[tts-worker] fatal: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
