find_package(Threads REQUIRED)

add_executable(zmq_test zmq_test.cpp)
target_link_libraries(zmq_test PRIVATE zmq_comm)

add_executable(stress_zmq_test stress_zmq.cpp)
target_link_libraries(stress_zmq_test 
    PRIVATE 
        zmq_comm 
        Threads::Threads
)

add_executable(tcp_server server.cpp)
target_link_libraries(tcp_server 
    PRIVATE 
        zmq_comm 
        Threads::Threads
)

add_executable(tcp_client client.cpp)
target_link_libraries(tcp_client 
    PRIVATE 
        zmq_comm 
        Threads::Threads
)

add_test(NAME StandardZmqTest COMMAND zmq_test)
add_test(NAME StressZmqTest COMMAND stress_zmq_test)