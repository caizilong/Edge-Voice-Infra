cmake_minimum_required(VERSION 3.10)
project(edge_voice_infra_llm_service)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

add_executable(llm_ipc_service src/ipc_service.cpp)

target_include_directories(llm_ipc_service PRIVATE
  ${REPO_ROOT}/common/stackflow/include
  ${REPO_ROOT}/common/zmq-comm/include
  ${REPO_ROOT}/third-party
  ${REPO_ROOT}/logging
)

target_link_libraries(llm_ipc_service PRIVATE stackflow)

set(RKLLM_API_PATH "${REPO_ROOT}/third-party/rknn-llm/rkllm-runtime/${CMAKE_SYSTEM_NAME}/librkllm_api")
if(EXISTS "${RKLLM_API_PATH}/include/rkllm.h")
  add_executable(llm_service src/main.cpp)
  target_include_directories(llm_service PRIVATE ${RKLLM_API_PATH}/include)

  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(RKLLM_RT_LIB ${RKLLM_API_PATH}/aarch64/librkllmrt.so)
    target_link_libraries(llm_service ${RKLLM_RT_LIB})
  elseif(CMAKE_SYSTEM_NAME STREQUAL "Android")
    set(RKLLM_RT_LIB ${RKLLM_API_PATH}/${CMAKE_ANDROID_ARCH_ABI}/librkllmrt.so)
    find_package(OpenMP REQUIRED)
    target_link_libraries(llm_service ${RKLLM_RT_LIB} log OpenMP::OpenMP_CXX)
  endif()
else()
  message(STATUS "RKLLM runtime not found; building llm_ipc_service only")
endif()
