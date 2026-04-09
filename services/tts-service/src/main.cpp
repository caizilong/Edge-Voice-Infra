#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "Hanz2Piny.h"
#include "SynthesizerTrn.h"
#include "utils.h"

namespace {

struct AppConfig {
  std::string model_path;
  std::string output_wav = "tts_output.wav";
  std::string text;
  std::string text_file;
  int speaker_id = 0;
  float length_scale = 1.0f;
};

void print_usage(const char* prog) {
  std::cout << "Usage: " << prog
            << " --model <model.bin> [--text \"你好\"] [--text-file <utf8.txt>]\n"
               "       [--output <output.wav>] [--speaker-id <id>] [--length-scale <float>]\n\n"
               "Examples:\n"
               "  "
            << prog
            << " --model ../../models/single_speaker_fast.bin --text \"你好，欢迎使用TTS\"\n"
               "  "
            << prog
            << " --model ../../models/single_speaker_fast.bin --text-file input.txt --output out.wav\n";
}

bool file_exists(const std::string& path) {
  std::ifstream ifs(path, std::ios::binary);
  return ifs.good();
}

bool parse_args(int argc, char** argv, AppConfig* cfg) {
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--model" && i + 1 < argc) {
      cfg->model_path = argv[++i];
    } else if (arg == "--output" && i + 1 < argc) {
      cfg->output_wav = argv[++i];
    } else if (arg == "--text" && i + 1 < argc) {
      cfg->text = argv[++i];
    } else if (arg == "--text-file" && i + 1 < argc) {
      cfg->text_file = argv[++i];
    } else if (arg == "--speaker-id" && i + 1 < argc) {
      cfg->speaker_id = std::atoi(argv[++i]);
    } else if (arg == "--length-scale" && i + 1 < argc) {
      cfg->length_scale = std::atof(argv[++i]);
    } else if (arg == "--help" || arg == "-h") {
      print_usage(argv[0]);
      return false;
    } else {
      std::cerr << "[tts-service] unknown arg: " << arg << "\n";
      print_usage(argv[0]);
      return false;
    }
  }

  if (cfg->model_path.empty()) {
    std::cerr << "[tts-service] --model is required\n";
    return false;
  }
  if (cfg->text.empty() && cfg->text_file.empty()) {
    std::cerr << "[tts-service] provide --text or --text-file\n";
    return false;
  }
  return true;
}

std::string read_utf8_text_file(const std::string& path) {
  const Hanz2Piny hanz2piny;
  if (!hanz2piny.isUtf8File(path)) {
    throw std::runtime_error("input text file is not valid UTF-8: " + path);
  }

  std::ifstream ifs(path);
  if (!ifs.is_open()) {
    throw std::runtime_error("failed to open text file: " + path);
  }

  std::string result;
  std::string line;
  while (std::getline(ifs, line)) {
    if (hanz2piny.isStartWithBom(line)) {
      line = std::string(line.cbegin() + 3, line.cend());
    }
    if (!line.empty()) {
      if (!result.empty()) {
        result += " ";
      }
      result += line;
    }
  }
  return result;
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

int run_tts(const AppConfig& cfg) {
  if (!file_exists(cfg.model_path)) {
    std::cerr << "[tts-service] model not found: " << cfg.model_path << "\n";
    return 1;
  }

  std::string text = cfg.text;
  if (text.empty()) {
    text = read_utf8_text_file(cfg.text_file);
  }
  if (text.empty()) {
    std::cerr << "[tts-service] input text is empty\n";
    return 1;
  }

  float* model_data = nullptr;
  int32_t model_size = ttsLoadModel(const_cast<char*>(cfg.model_path.c_str()), &model_data);
  if (model_size <= 0 || model_data == nullptr) {
    std::cerr << "[tts-service] failed to load model: " << cfg.model_path << "\n";
    return 1;
  }

  std::unique_ptr<SynthesizerTrn> synthesizer(new SynthesizerTrn(model_data, model_size));
  const int speaker_num = synthesizer->getSpeakerNum();
  std::cout << "[tts-service] model loaded, speakers=" << speaker_num << "\n";

  const int speaker = (cfg.speaker_id >= 0 && cfg.speaker_id < speaker_num) ? cfg.speaker_id : 0;
  if (speaker != cfg.speaker_id) {
    std::cout << "[tts-service] speaker-id out of range, fallback to 0\n";
  }

  int32_t sample_count = 0;
  int16_t* pcm = synthesizer->infer(text, speaker, cfg.length_scale, sample_count);
  if (pcm == nullptr || sample_count <= 0) {
    std::cerr << "[tts-service] synthesize failed\n";
    tts_free_data(model_data);
    return 1;
  }

  std::vector<char> wav = build_wav_buffer(pcm, sample_count);
  std::ofstream ofs(cfg.output_wav, std::ios::binary);
  if (!ofs.is_open()) {
    std::cerr << "[tts-service] failed to open output: " << cfg.output_wav << "\n";
    tts_free_data(pcm);
    tts_free_data(model_data);
    return 1;
  }
  ofs.write(wav.data(), static_cast<std::streamsize>(wav.size()));
  ofs.close();

  tts_free_data(pcm);
  tts_free_data(model_data);
  std::cout << "[tts-service] wav generated: " << cfg.output_wav << "\n";
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  AppConfig cfg;
  if (!parse_args(argc, argv, &cfg)) {
    return (argc > 1 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h")) ? 0 : 1;
  }

  try {
    return run_tts(cfg);
  } catch (const std::exception& e) {
    std::cerr << "[tts-service] exception: " << e.what() << "\n";
    return 1;
  }
}

