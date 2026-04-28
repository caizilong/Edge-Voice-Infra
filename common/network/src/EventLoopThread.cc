#include "EventLoopThread.h"

#include <pthread.h>

#include "EventLoop.h"

namespace network
{
  EventLoopThread::EventLoopThread(const ThreadInitCallback &cb,
                                   const std::string &name)
      : loop_(nullptr),
        exiting_(false),
        mutex_(),
        callback_(cb),
        name_(name) {}

  EventLoopThread::~EventLoopThread()
  {
    exiting_ = true;
    if (loop_ !=
        nullptr) // not 100% race-free, eg. threadFunc could be running callback_.
    {
      // still a tiny chance to call destructed object, if threadFunc exits just
      // now. but when EventLoopThread destructs, usually programming is exiting
      // anyway.
      loop_->quit();
    }
  }

  EventLoop *EventLoopThread::startLoop()
  {
    // assert(!thread_.started());
    // thread_.start();
    thread_ = std::jthread([this]() { threadFunc(); });
    EventLoop *loop = nullptr;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      while (loop_ == nullptr)
      {
        cv_.wait(lock);
      }
      loop = loop_;
    }

    return loop;
  }

  void EventLoopThread::threadFunc()
  {
    const std::string thread_name =
        name_.empty() ? std::string("EventLoopThread") : name_.substr(0, 15);
    pthread_setname_np(pthread_self(), thread_name.c_str());
    EventLoop loop;

    if (callback_)
    {
      callback_(&loop);
    }

    {
      std::unique_lock<std::mutex> lock(mutex_);
      loop_ = &loop;
      cv_.notify_all();
    }

    loop.loop();
    // assert(exiting_);
    std::unique_lock<std::mutex> lock(mutex_);
    loop_ = nullptr;
  }
} // namespace network
