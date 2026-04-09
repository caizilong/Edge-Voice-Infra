#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <algorithm>
#include <clocale>
#include <cwctype>
#include <iostream>
#include <string>
#include <vector>

#include "portaudio.h"  // NOLINT
#include "sherpa-onnx/csrc/display.h"
#include "sherpa-onnx/csrc/microphone.h"
#include "sherpa-onnx/csrc/online-recognizer.h"

namespace {

bool g_stop = false;
float g_mic_sample_rate = 16000;

int32_t RecordCallback(const void* input_buffer, void* /*output_buffer*/,
                       unsigned long frames_per_buffer,   // NOLINT
                       const PaStreamCallbackTimeInfo* /*time_info*/,
                       PaStreamCallbackFlags /*status_flags*/, void* user_data) {
  auto stream = reinterpret_cast<sherpa_onnx::OnlineStream*>(user_data);
  stream->AcceptWaveform(g_mic_sample_rate,
                         reinterpret_cast<const float*>(input_buffer),
                         frames_per_buffer);
  return g_stop ? paComplete : paContinue;
}

void Handler(int32_t /*sig*/) {
  g_stop = true;
  fprintf(stderr, "\nCaught Ctrl + C. Exiting...\n");
}

std::string ToLowerUnicode(const std::string& input_str) {
  std::setlocale(LC_ALL, "");
  std::wstring input_wstr(input_str.size() + 1, '\0');
  std::mbstowcs(&input_wstr[0], input_str.c_str(), input_str.size());
  std::wstring lowercase_wstr;

  for (wchar_t wc : input_wstr) {
    if (std::iswupper(wc)) {
      lowercase_wstr += std::towlower(wc);
    } else {
      lowercase_wstr += wc;
    }
  }

  std::string lowercase_str(input_str.size() + 1, '\0');
  std::wcstombs(&lowercase_str[0], lowercase_wstr.c_str(),
                lowercase_wstr.size());
  return lowercase_str;
}

}  // namespace

int32_t main(int32_t argc, char* argv[]) {
  signal(SIGINT, Handler);

  const char* kUsageMessage = R"usage(
This program uses streaming models with microphone for speech recognition.
Usage:

  ./voice_microphone \
    --tokens=/path/to/tokens.txt \
    --encoder=/path/to/encoder.onnx \
    --decoder=/path/to/decoder.onnx \
    --joiner=/path/to/joiner.onnx \
    --provider=cpu \
    --num-threads=1 \
    --decoding-method=greedy_search
)usage";

  sherpa_onnx::ParseOptions po(kUsageMessage);
  sherpa_onnx::OnlineRecognizerConfig config;
  config.Register(&po);
  po.Read(argc, argv);

  if (po.NumArgs() != 0) {
    po.PrintUsage();
    exit(EXIT_FAILURE);
  }

  fprintf(stderr, "%s\n", config.ToString().c_str());
  if (!config.Validate()) {
    fprintf(stderr, "Errors in config!\n");
    return -1;
  }

  sherpa_onnx::OnlineRecognizer recognizer(config);
  auto stream_obj = recognizer.CreateStream();
  sherpa_onnx::Microphone mic;

  const int32_t num_devices = Pa_GetDeviceCount();
  fprintf(stderr, "Num devices: %d\n", num_devices);

  int32_t device_index = Pa_GetDefaultInputDevice();
  if (device_index == paNoDevice) {
    fprintf(stderr, "No default input device found\n");
    fprintf(stderr, "If you are using Linux, please switch to ./bin/sherpa-onnx-alsa\n");
    exit(EXIT_FAILURE);
  }

  const char* pDeviceIndex = std::getenv("SHERPA_ONNX_MIC_DEVICE");
  if (pDeviceIndex) {
    fprintf(stderr, "Use specified device: %s\n", pDeviceIndex);
    device_index = atoi(pDeviceIndex);
  }

  for (int32_t i = 0; i != num_devices; ++i) {
    const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
    fprintf(stderr, " %s %d %s\n", (i == device_index) ? "*" : " ", i, info->name);
  }

  const char* pSampleRateStr = std::getenv("SHERPA_ONNX_MIC_SAMPLE_RATE");
  if (pSampleRateStr) {
    g_mic_sample_rate = atof(pSampleRateStr);
    fprintf(stderr, "Use sample rate %f for mic\n", g_mic_sample_rate);
  }

  if (!mic.OpenDevice(device_index, 16000, 1, RecordCallback, stream_obj.get())) {
    fprintf(stderr, "portaudio error: %d\n", device_index);
    exit(EXIT_FAILURE);
  }

  std::string last_text;
  int32_t segment_index = 0;
  sherpa_onnx::Display display(30);

  while (!g_stop) {
    while (recognizer.IsReady(stream_obj.get())) {
      recognizer.DecodeStream(stream_obj.get());
    }

    auto text = recognizer.GetResult(stream_obj.get()).text;
    bool is_endpoint = recognizer.IsEndpoint(stream_obj.get());

    if (is_endpoint && !config.model_config.paraformer.encoder.empty()) {
      std::vector<float> tail_paddings(static_cast<int>(1.0 * g_mic_sample_rate));
      stream_obj->AcceptWaveform(g_mic_sample_rate, tail_paddings.data(),
                                 tail_paddings.size());
      while (recognizer.IsReady(stream_obj.get())) {
        recognizer.DecodeStream(stream_obj.get());
      }
      text = recognizer.GetResult(stream_obj.get()).text;
    }

    if (!text.empty() && last_text != text) {
      last_text = text;
      display.Print(segment_index, ToLowerUnicode(text));
      fflush(stderr);
    }

    if (is_endpoint) {
      if (!text.empty()) {
        ++segment_index;
      }
      recognizer.Reset(stream_obj.get());
    }

    Pa_Sleep(20);
  }

  return 0;
}
