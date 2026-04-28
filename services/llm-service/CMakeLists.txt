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
find_path(RKLLM_INCLUDE_DIR
  NAMES rkllm.h
  PATHS
    "${RKLLM_API_PATH}/include"
    "${REPO_ROOT}/third-party/rknn-llm/include"
    "/usr/local/include"
    "/usr/include"
  NO_DEFAULT_PATH
)
find_library(RKLLM_RT_LIB
  NAMES rkllmrt librkllmrt.so
  PATHS
    "${RKLLM_API_PATH}/aarch64"
    "${RKLLM_API_PATH}"
    "${REPO_ROOT}/third-party/rknn-llm/lib"
    "/usr/local/lib"
    "/usr/lib"
  NO_DEFAULT_PATH
)

if(RKLLM_INCLUDE_DIR AND RKLLM_RT_LIB)
  target_compile_definitions(llm_ipc_service PRIVATE EDGE_ENABLE_RKLLM=1)
  target_include_directories(llm_ipc_service PRIVATE ${RKLLM_INCLUDE_DIR})
  target_link_libraries(llm_ipc_service PRIVATE ${RKLLM_RT_LIB})

  add_executable(llm_service src/main.cpp)
  target_include_directories(llm_service PRIVATE ${RKLLM_INCLUDE_DIR})

  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(llm_service ${RKLLM_RT_LIB})
  elseif(CMAKE_SYSTEM_NAME STREQUAL "Android")
    find_package(OpenMP REQUIRED)
    target_link_libraries(llm_service ${RKLLM_RT_LIB} log OpenMP::OpenMP_CXX)
  endif()
else()
  message(STATUS "RKLLM runtime not found; llm_ipc_service will use mock backend")
endif()
