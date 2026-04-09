cmake_minimum_required(VERSION 3.10)
project(edge_voice_infra_llm_service)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(llm_service src/main.cpp)

set(RKLLM_API_PATH "${CMAKE_SOURCE_DIR}/../../third-party/rknn-llm/rkllm-runtime/${CMAKE_SYSTEM_NAME}/librkllm_api")
include_directories(${RKLLM_API_PATH}/include)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(RKLLM_RT_LIB ${RKLLM_API_PATH}/aarch64/librkllmrt.so)
  target_link_libraries(llm_service ${RKLLM_RT_LIB})
elseif(CMAKE_SYSTEM_NAME STREQUAL "Android")
  set(RKLLM_RT_LIB ${RKLLM_API_PATH}/${CMAKE_ANDROID_ARCH_ABI}/librkllmrt.so)
  find_package(OpenMP REQUIRED)
  target_link_libraries(llm_service ${RKLLM_RT_LIB} log OpenMP::OpenMP_CXX)
else()
  message(FATAL_ERROR "Unsupported system: ${CMAKE_SYSTEM_NAME}")
endif()
