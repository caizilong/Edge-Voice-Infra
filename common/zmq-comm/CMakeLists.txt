add_library(zmq_comm
    src/zmq_message.cpp
    src/zmq_endpoint.cpp
)

target_include_directories(zmq_comm PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include/libzmq
)

target_link_libraries(zmq_comm PUBLIC 
    zmq 
    pthread
)