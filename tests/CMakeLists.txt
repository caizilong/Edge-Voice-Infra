add_executable(zmq_test zmq_test.cpp)

target_link_libraries(zmq_test PRIVATE zmq_comm)