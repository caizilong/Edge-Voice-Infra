# 1. 生成可执行文件
add_executable(zmq_test zmq_test.cpp)

# 2. 链接我们在 common/zmq-comm 里编译好的 zmq_comm 库
# 因为在 zmq_comm 里设置了 PUBLIC 属性，这里链接后，test.cpp 就能自动找到 zmq_endpoint.h 了！
target_link_libraries(zmq_test PRIVATE zmq_comm)