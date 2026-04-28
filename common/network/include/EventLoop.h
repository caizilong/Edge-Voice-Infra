#pragma once

#include <sys/syscall.h>

#include <any>
#include <atomic>
#include <coroutine>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "Callbacks.h"
#include "util.h"
namespace network {

class Channel;
class Poller;

///
/// Reactor, at most one per thread.
///
/// This is an interface class, so don't expose too much details.
class EventLoop {
 public:
  using Functor = std::function<void()>;

  class ScheduleAwaiter {
   public:
    explicit ScheduleAwaiter(EventLoop *loop) noexcept : loop_(loop) {}

    bool await_ready() const noexcept {
      return loop_ == nullptr || loop_->isInLoopThread();
    }

    void await_suspend(std::coroutine_handle<> handle) const {
      loop_->queueInLoop([handle]() mutable { handle.resume(); });
    }

    void await_resume() const noexcept {}

   private:
    EventLoop *loop_;
  };

  EventLoop();
  ~EventLoop();  // force out-line dtor, for std::unique_ptr members.

  ///
  /// Loops forever.
  ///
  /// Must be called in the same thread as creation of the object.
  ///
  void loop();

  /// Quits loop.
  ///
  /// This is not 100% thread safe, if you call through a raw pointer,
  /// better to call through shared_ptr<EventLoop> for 100% safety.
  void quit();

  int64_t iteration() const { return iteration_; }

  /// Runs callback immediately in the loop thread.
  /// It wakes up the loop, and run the cb.
  /// If in the same loop thread, cb is run within the function.
  /// Safe to call from other threads.
  void runInLoop(Functor cb);
  /// Queues callback in the loop thread.
  /// Runs after finish pooling.
  /// Safe to call from other threads.
  void queueInLoop(Functor cb);

  /// Resume current coroutine in loop thread.
  [[nodiscard]] ScheduleAwaiter schedule() noexcept {
    return ScheduleAwaiter(this);
  }

  /// Resume a suspended coroutine in loop thread.
  void resumeInLoop(std::coroutine_handle<> handle);

  size_t queueSize() const;

  // internal usage
  void wakeup();
  void updateChannel(Channel *channel);
  void removeChannel(Channel *channel);
  bool hasChannel(Channel *channel);

  // pid_t threadId() const { return threadId_; }
  void assertInLoopThread() {
    if (!isInLoopThread()) {
      abortNotInLoopThread();
    }
  }

  bool isInLoopThread() const { return threadId_ == getThreadId(); }
  // bool callingPendingFunctors() const { return callingPendingFunctors_; }
  bool eventHandling() const { return eventHandling_; }

  void setContext(const std::any &context) { context_ = context; }

  const std::any &getContext() const { return context_; }

  std::any *getMutableContext() { return &context_; }

  static EventLoop *getEventLoopOfCurrentThread();

 private:
  void abortNotInLoopThread();
  void handleRead();  // waked up
  void doPendingFunctors();

  void printActiveChannels() const;  // DEBUG

  using ChannelList = std::vector<Channel *>;

  bool looping_; /* atomic */
  std::atomic<bool> quit_;
  bool eventHandling_;          /* atomic */
  bool callingPendingFunctors_; /* atomic */
  int64_t iteration_;
  pid_t threadId_;
  std::unique_ptr<Poller> poller_;
  int wakeupFd_;
  // unlike in TimerQueue, which is an internal class,
  // we don't expose Channel to client.
  std::unique_ptr<Channel> wakeupChannel_;
  std::any context_;

  // scratch variables
  ChannelList activeChannels_;
  Channel *currentActiveChannel_;

  mutable std::mutex mutex_;
  std::vector<Functor> pendingFunctors_;
};

}  // namespace network
