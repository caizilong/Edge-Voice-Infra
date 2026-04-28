#pragma once

#include <coroutine>
#include <deque>
#include <mutex>

#include "Task.h"
#include "TcpConnection.h"
namespace network {

class Connector;
using ConnectorPtr = std::shared_ptr<Connector>;

class TcpClient {
 public:
  // TcpClient(EventLoop* loop);
  // TcpClient(EventLoop* loop, const string& host, uint16_t port);
  TcpClient(EventLoop *loop, const InetAddress &serverAddr,
            const std::string &nameArg);
  ~TcpClient();  // force out-line dtor, for std::unique_ptr members.

  void connect();
  // Starts connection and suspends until a TcpConnection is established.
  Task<TcpConnectionPtr> connectAsync();
  void disconnect();
  void stop();

  class ConnectAwaiter {
   public:
    explicit ConnectAwaiter(TcpClient *client) noexcept : client_(client) {}

    bool await_ready();
    void await_suspend(std::coroutine_handle<> handle);
    TcpConnectionPtr await_resume();

   private:
    TcpClient *client_;
    TcpConnectionPtr readyConnection_;
  };

  [[nodiscard]] ConnectAwaiter asyncConnect() noexcept {
    return ConnectAwaiter(this);
  }

  TcpConnectionPtr connection() const {
    std::unique_lock<std::mutex> lock(mutex_);
    return connection_;
  }

  EventLoop *getLoop() const { return loop_; }
  bool retry() const { return retry_; }
  void enableRetry() { retry_ = true; }

  const std::string &name() const { return name_; }

  /// Set connection callback.
  /// Not thread safe.
  void setConnectionCallback(ConnectionCallback cb) {
    connectionCallback_ = std::move(cb);
  }

  /// Set message callback.
  /// Not thread safe.
  void setMessageCallback(MessageCallback cb) {
    messageCallback_ = std::move(cb);
  }

  /// Set write complete callback.
  /// Not thread safe.
  void setWriteCompleteCallback(WriteCompleteCallback cb) {
    writeCompleteCallback_ = std::move(cb);
  }

 private:
  /// Not thread safe, but in loop
  void newConnection(int sockfd);
  /// Not thread safe, but in loop
  void removeConnection(const TcpConnectionPtr &conn);
  bool tryPopConnected(TcpConnectionPtr &conn);
  void enqueueConnectWaiter(std::coroutine_handle<> handle);
  void publishConnected(const TcpConnectionPtr &conn);

  EventLoop *loop_;
  ConnectorPtr connector_;  // avoid revealing Connector
  const std::string name_;
  ConnectionCallback connectionCallback_;
  MessageCallback messageCallback_;
  WriteCompleteCallback writeCompleteCallback_;
  bool retry_;    // atomic
  bool connect_;  // atomic
  // always in loop thread
  int nextConnId_;
  mutable std::mutex mutex_;
  TcpConnectionPtr connection_;
  std::mutex connectAwaiterMutex_;
  std::deque<TcpConnectionPtr> connectedQueue_;
  std::deque<std::coroutine_handle<>> connectWaiters_;
};

}  // namespace network
