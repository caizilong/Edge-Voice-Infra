cmake_minimum_required(VERSION 3.20)
project(stackflow)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

file(GLOB SRCS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_library(stackflow STATIC ${SRCS})

target_include_directories(stackflow PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/common/zmq-comm/include
    ${CMAKE_SOURCE_DIR}/third-party
)

find_package(eventpp REQUIRED)

target_link_libraries(stackflow PUBLIC
    zmq_comm
    eventpp::eventpp 
)

install(TARGETS stackflow
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)