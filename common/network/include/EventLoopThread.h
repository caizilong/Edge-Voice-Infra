#pragma once

#include <cassert>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace network {

class EventLoop;

class EventLoopThread {
 public:
  using ThreadInitCallback = std::function<void(EventLoop *)>;

  EventLoopThread(const ThreadInitCallback &cb = ThreadInitCallback(),
                  const std::string &name = std::string());
  ~EventLoopThread();
  EventLoop *startLoop();

 private:
  void threadFunc();

  EventLoop *loop_;
  bool exiting_;
  std::jthread thread_;
  std::mutex mutex_;
  std::condition_variable cv_;
  ThreadInitCallback callback_;
  std::string name_;
};

}  // namespace network
