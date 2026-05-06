cmake_minimum_required(VERSION 3.16)
project(edge_voice_infra_tts_service)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

add_executable(tts_ipc_service src/ipc_service.cpp)

target_include_directories(tts_ipc_service PRIVATE
  ${REPO_ROOT}/common/stackflow/include
  ${REPO_ROOT}/common/zmq-comm/include
  ${REPO_ROOT}/third-party
  ${REPO_ROOT}/logging
)

target_link_libraries(tts_ipc_service PRIVATE stackflow)

set(SUMMERTTS_ROOT "${REPO_ROOT}/third-party/SummerTTS")

if(EXISTS "${SUMMERTTS_ROOT}/include/SynthesizerTrn.h")

set(SUMMERTTS_GLOG_SRC
  "${SUMMERTTS_ROOT}/src/tn/glog/src/demangle.cc"
  "${SUMMERTTS_ROOT}/src/tn/glog/src/logging.cc"
  "${SUMMERTTS_ROOT}/src/tn/glog/src/raw_logging.cc"
  "${SUMMERTTS_ROOT}/src/tn/glog/src/symbolize.cc"
  "${SUMMERTTS_ROOT}/src/tn/glog/src/utilities.cc"
  "${SUMMERTTS_ROOT}/src/tn/glog/src/vlog_is_on.cc"
  "${SUMMERTTS_ROOT}/src/tn/glog/src/signalhandler.cc"
)
set(SUMMERTTS_GFLAGS_SRC
  "${SUMMERTTS_ROOT}/src/tn/gflags/src/gflags.cc"
  "${SUMMERTTS_ROOT}/src/tn/gflags/src/gflags_reporting.cc"
  "${SUMMERTTS_ROOT}/src/tn/gflags/src/gflags_completions.cc"
)
set(SUMMERTTS_OPENFST_SRC
  "${SUMMERTTS_ROOT}/src/tn/openfst/src/lib/compat.cc"
  "${SUMMERTTS_ROOT}/src/tn/openfst/src/lib/flags.cc"
  "${SUMMERTTS_ROOT}/src/tn/openfst/src/lib/fst.cc"
  "${SUMMERTTS_ROOT}/src/tn/openfst/src/lib/fst-types.cc"
  "${SUMMERTTS_ROOT}/src/tn/openfst/src/lib/mapped-file.cc"
  "${SUMMERTTS_ROOT}/src/tn/openfst/src/lib/properties.cc"
  "${SUMMERTTS_ROOT}/src/tn/openfst/src/lib/symbol-table.cc"
  "${SUMMERTTS_ROOT}/src/tn/openfst/src/lib/symbol-table-ops.cc"
  "${SUMMERTTS_ROOT}/src/tn/openfst/src/lib/util.cc"
  "${SUMMERTTS_ROOT}/src/tn/openfst/src/lib/weight.cc"
)
set(SUMMERTTS_TN_SRC
  "${SUMMERTTS_ROOT}/src/tn/processor.cc"
  "${SUMMERTTS_ROOT}/src/tn/token_parser.cc"
  "${SUMMERTTS_ROOT}/src/tn/utf8_string.cc"
)
file(GLOB SUMMERTTS_ENGIPA_SRC "${SUMMERTTS_ROOT}/src/engipa/*.cpp")
file(GLOB SUMMERTTS_HZ2PY_SRC "${SUMMERTTS_ROOT}/src/hz2py/*.cpp")
file(GLOB SUMMERTTS_NN_SRC "${SUMMERTTS_ROOT}/src/nn_op/*.cpp")
file(GLOB SUMMERTTS_PLATFORM_SRC "${SUMMERTTS_ROOT}/src/platform/*.cpp")
file(GLOB SUMMERTTS_UTILS_SRC "${SUMMERTTS_ROOT}/src/utils/*.cpp")
file(GLOB SUMMERTTS_MODULES_SRC "${SUMMERTTS_ROOT}/src/modules/*.cpp")
file(GLOB SUMMERTTS_MODELS_SRC "${SUMMERTTS_ROOT}/src/models/*.cpp")

set(SUMMERTTS_SRC
  ${SUMMERTTS_GLOG_SRC}
  ${SUMMERTTS_GFLAGS_SRC}
  ${SUMMERTTS_OPENFST_SRC}
  ${SUMMERTTS_TN_SRC}
  ${SUMMERTTS_ENGIPA_SRC}
  ${SUMMERTTS_HZ2PY_SRC}
  ${SUMMERTTS_NN_SRC}
  ${SUMMERTTS_PLATFORM_SRC}
  ${SUMMERTTS_UTILS_SRC}
  ${SUMMERTTS_MODULES_SRC}
  ${SUMMERTTS_MODELS_SRC}
)

add_executable(edge_tts_service
  src/main.cpp
  ${SUMMERTTS_SRC}
)

set_target_properties(edge_tts_service PROPERTIES
  CXX_STANDARD 14
  CXX_STANDARD_REQUIRED ON
)

target_include_directories(edge_tts_service PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${SUMMERTTS_ROOT}/include
  ${SUMMERTTS_ROOT}/src/header
  ${SUMMERTTS_ROOT}/src/tn/header
  ${SUMMERTTS_ROOT}/eigen-3.4.0
)

target_compile_options(edge_tts_service PRIVATE
  -O3
  -fopenmp
  -include stdint.h
  -Wno-stringop-overflow
)

find_package(OpenMP)
if(OpenMP_CXX_FOUND)
  target_link_libraries(edge_tts_service OpenMP::OpenMP_CXX)
endif()

else()
  message(STATUS "SummerTTS not found; building tts_ipc_service only")
endif()
