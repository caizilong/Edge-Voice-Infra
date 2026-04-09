cmake_minimum_required(VERSION 3.16)
project(edge_voice_infra_voice_service)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Reuse sherpa-onnx core from third-party without modifying upstream source.
set(SHERPA_ONNX_ENABLE_BINARY OFF CACHE BOOL "" FORCE)
set(SHERPA_ONNX_ENABLE_PORTAUDIO ON CACHE BOOL "" FORCE)
set(SHERPA_ONNX_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(SHERPA_ONNX_ENABLE_CHECK OFF CACHE BOOL "" FORCE)

add_subdirectory(
  ${CMAKE_SOURCE_DIR}/../../third-party/sherpa-onnx
  ${CMAKE_BINARY_DIR}/third_party_sherpa
)

add_executable(edge_voice_microphone
  src/voice_microphone.cpp
  ${CMAKE_SOURCE_DIR}/../../third-party/sherpa-onnx/sherpa-onnx/csrc/microphone.cc
)

target_include_directories(edge_voice_microphone PRIVATE
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/../../third-party/sherpa-onnx
)

target_link_libraries(edge_voice_microphone
  sherpa-onnx-core
  portaudio_static
  pthread
)
