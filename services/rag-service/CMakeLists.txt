cmake_minimum_required(VERSION 3.20)
project(edge_voice_infra_rag_service)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

add_executable(rag_ipc_service
  src/main.cpp
)

target_include_directories(rag_ipc_service PRIVATE
  ${REPO_ROOT}/common/stackflow/include
  ${REPO_ROOT}/common/zmq-comm/include
  ${REPO_ROOT}/third-party
  ${REPO_ROOT}/logging
)

target_link_libraries(rag_ipc_service PRIVATE
  stackflow
)
