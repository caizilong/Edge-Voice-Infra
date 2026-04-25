cmake_minimum_required(VERSION 3.10)
project(EdgeVoiceInfra)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

enable_testing()

add_subdirectory(common/zmq-comm)

add_subdirectory(tests)