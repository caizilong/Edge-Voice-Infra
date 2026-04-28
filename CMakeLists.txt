cmake_minimum_required(VERSION 3.20)
project(EdgeVoiceInfra)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

enable_testing()

add_subdirectory(common/zmq-comm)

option(BUILD_STACKFLOW "Build stackflow common module" ON)
if(BUILD_STACKFLOW)
	add_subdirectory(common/stackflow)
else()
	message(STATUS "Skipping build of common/stackflow (use -DBUILD_STACKFLOW=ON to enable)")
endif()

option(BUILD_GATEWAY "Build gateway service" ON)
if(BUILD_GATEWAY)
	add_subdirectory(common/gateway)
endif()

option(BUILD_PHASE1_SERVICES "Build phase1 IPC business services" ON)
if(BUILD_PHASE1_SERVICES AND BUILD_STACKFLOW)
	add_subdirectory(services/rag-service)
	add_subdirectory(services/llm-service)
	add_subdirectory(services/tts-service)
endif()

add_subdirectory(tests)
