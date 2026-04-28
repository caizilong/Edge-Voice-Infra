include(CheckFunctionExists)

check_function_exists(accept4 HAVE_ACCEPT4)
if(NOT HAVE_ACCEPT4)
  set_source_files_properties(src/SocketsOps.cc PROPERTIES COMPILE_FLAGS "-DNO_ACCEPT4")
endif()

set(net_SRCS
  src/Acceptor.cc
  src/buffer.cc
  src/Channel.cc
  src/Connector.cc
  src/EventLoop.cc
  src/EventLoopThread.cc
  src/EventLoopThreadPool.cc
  src/InetAddress.cc
  src/Poller.cc
  src/Socket.cc
  src/SocketsOps.cc
  src/TcpClient.cc
  src/TcpConnection.cc
  src/TcpServer.cc
  src/util.cc
  )

add_library(network ${net_SRCS})
target_compile_features(network PUBLIC cxx_std_20)
target_link_libraries(network  glog)
target_include_directories(network PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  )


install(TARGETS  network DESTINATION  ${PROJECT_BINARY_DIR}/lib)
