# 1. 编译静态/动态库 (如果你的 endpoint 也写在 message 里，就只留 zmq_message.cpp)
add_library(zmq_comm
    src/zmq_message.cpp
    src/zmq_endpoint.cpp
)

# 2. 设置头文件包含路径 (极其重要：使用 PUBLIC)
# 使用 PUBLIC 的意思是：不仅我自己编译时需要这些头文件，谁链接了我(zmq_comm)，谁也能自动获取这些头文件路径！
target_include_directories(zmq_comm PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include/libzmq
)

# 3. 链接系统的 ZeroMQ 和线程库 (同样使用 PUBLIC)
target_link_libraries(zmq_comm PUBLIC 
    zmq 
    pthread
)