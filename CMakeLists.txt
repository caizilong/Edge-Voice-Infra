# my_project/CMakeLists.txt
cmake_minimum_required(VERSION 3.10)
project(MyRootProject) # 给你的总项目起个名字

# 全局设置 C++ 标准
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 1. 优先添加底层基础库所在目录
add_subdirectory(common/zmq-comm)

# 2. 添加测试程序所在目录
add_subdirectory(tests)