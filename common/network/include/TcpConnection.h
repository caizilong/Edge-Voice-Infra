#pragma once

#include <any>
#include <coroutine>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>

#include "network/Buffer.h"
#include "network/Callbacks.h"
#include "network/InetAddress.h"
#include "network/Task.h"

// struct tcp_info is in <netinet/tcp.h>
struct tcp_info;

namespace network {

class Channel;
class EventLoop;
class Socket;

///
/// TCP connection, for both client and server usage.
///
/// This is an interface class, so don't expose too much details.
class TcpConnection : public std::enable_shared_from_this<TcpConnection> {
 public:
  /// Constructs a TcpConnection with a connected sockfd
  ///
  /// User should not create this object.
  TcpConnection(EventLoop *loop, const std::string &name, int sockfd,
                const InetAddress &localAddr, const InetAddress &peerAddr);
  ~TcpConnection();

  EventLoop *getLoop() const { return loop_; }
  const std::string &name() const { return name_; }
  const InetAddress &localAddress() const { return localAddr_; }
  const InetAddress &peerAddress() const { return peerAddr_; }
  bool connected() const { return state_ == kConnected; }
  bool disconnected() const { return state_ == kDisconnected; }
  // return true if success.
  bool getTcpInfo(struct tcp_info *) const;
  std::string getTcpInfoString() const;

  // void send(string&& message); // C++11
  // void send(const void *message, int len);
  // void send(const std::string &message);
  // void send(Buffer&& message); // C++11
  void send(Buffer *message);                 // this one will swap data
  void send(std::string_view message);        // lightweight API for callers
  Task<size_t> sendAsync(std::string message);  // coroutine API
  void shutdown();             // NOT thread safe, no simultaneous calling
  // void shutdownAndForceCloseAfter(double seconds); // NOT thread safe, no
  // simultaneous calling
  void forceClose();
  void forceCloseWithDelay(double seconds);
  void setTcpNoDelay(bool on);
  // reading or not
  void startRead();
  void stopRead();
  bool isReading() const {
    return reading_;
  };  // NOT thread safe, may race with start/stopReadInLoop

  class AsyncReadAwaiter {
   public:
    explicit AsyncReadAwaiter(TcpConnection *conn) noexcept : conn_(conn) {}

    bool await_ready();
    void await_suspend(std::coroutine_handle<> handle);
    std::optional<std::string> await_resume();

   private:
    TcpConnection *conn_;
    std::optional<std::string> readyChunk_;
  };

  [[nodiscard]] AsyncReadAwaiter asyncRead() noexcept {
    return AsyncReadAwaiter(this);
  }

  // Suspends until the next inbound chunk arrives, or returns std::nullopt
  // when the connection closes.
  Task<std::optional<std::string>> receiveAsync();

  void setContext(const std::any &context) { context_ = context; }

  const std::any &getContext() const { return context_; }

  std::any *getMutableContext() { return &context_; }

  void setConnectionCallback(const ConnectionCallback &cb) {
    connectionCallback_ = cb;
  }

  void setMessageCallback(const MessageCallback &cb) { messageCallback_ = cb; }

  void setWriteCompleteCallback(const WriteCompleteCallback &cb) {
    writeCompleteCallback_ = cb;
  }

  // void setHighWaterMarkCallback(const HighWaterMarkCallback &cb,
  //                               size_t highWaterMark) {
  //   highWaterMarkCallback_ = cb;
  //   highWaterMark_ = highWaterMark;
  // }

  /// Advanced interface
  Buffer *inputBuffer() { return &inputBuffer_; }

  Buffer *outputBuffer() { return &outputBuffer_; }

  /// Internal use only.
  void setCloseCallback(const CloseCallback &cb) { closeCallback_ = cb; }

  // called when TcpServer accepts a new connection
  void connectEstablished();  // should be called only once
  // called when TcpServer has removed me from its map
  void connectDestroyed();  // should be called only once

 private:
  enum StateE { kDisconnected, kConnecting, kConnected, kDisconnecting };
  void handleRead();
  void handleWrite();
  void handleClose();
  void handleError();
  // void sendInLoop(string&& message);
  void sendInLoop(const std::string &message);
  void sendInLoop(const void *message, size_t len);
  void shutdownInLoop();
  // void shutdownAndForceCloseInLoop(double seconds);
  void forceCloseInLoop();
  void setState(StateE s) { state_ = s; }
  const char *stateToString() const;
  void startReadInLoop();
  void stopReadInLoop();
  bool tryPopAsyncRead(std::optional<std::string> &chunk);
  void enqueueAsyncReadWaiter(std::coroutine_handle<> handle);
  void publishAsyncRead(std::string chunk);
  void publishAsyncClosed();

  EventLoop *loop_;
  const std::string name_;
  StateE state_;  // FIXME: use atomic variable
  bool reading_;
  // we don't expose those classes to client.
  std::unique_ptr<Socket> socket_;
  std::unique_ptr<Channel> channel_;
  const InetAddress localAddr_;
  const InetAddress peerAddr_;
  ConnectionCallback connectionCallback_;
  MessageCallback messageCallback_;
  WriteCompleteCallback writeCompleteCallback_;
  // HighWaterMarkCallback highWaterMarkCallback_;
  CloseCallback closeCallback_;
  // size_t highWaterMark_;
  Buffer inputBuffer_;
  Buffer outputBuffer_;  // FIXME: use list<Buffer> as output buffer.
  std::any context_;
  std::mutex asyncReadMutex_;
  std::deque<std::optional<std::string>> asyncReadQueue_;
  std::deque<std::coroutine_handle<>> asyncReadWaiters_;
  bool asyncReadClosed_ = false;
  // FIXME: creationTime_, lastReceiveTime_
  //        bytesReceived_, bytesSent_
};

using TcpConnectionPtr = std::shared_ptr<TcpConnection>;

}  // namespace network
