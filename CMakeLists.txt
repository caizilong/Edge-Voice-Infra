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

add_subdirectory(tests)