#include "network/TcpClient.h"

#include <stdio.h>  // snprintf

#include <cassert>
#include <utility>
#include <vector>

#include <glog/logging.h>

#include "network/Connector.h"
#include "network/EventLoop.h"
#include "network/SocketsOps.h"

using namespace network;

// TcpClient::TcpClient(EventLoop* loop)
//   : loop_(loop)
// {
// }

// TcpClient::TcpClient(EventLoop* loop, const std::string& host, uint16_t port)
//   : loop_(CHECK_NOTNULL(loop)),
//     serverAddr_(host, port)
// {
// }

namespace network {
namespace detail {

void removeConnection(EventLoop *loop, const TcpConnectionPtr &conn) {
  loop->queueInLoop([conn]() { conn->connectDestroyed(); });
}

void removeConnector(const ConnectorPtr &connector) {
  // connector->
}

}  // namespace detail

}  // namespace network

TcpClient::TcpClient(EventLoop *loop, const InetAddress &serverAddr,
                     const std::string &nameArg)
    : loop_(CHECK_NOTNULL(loop)),
      connector_(new Connector(loop, serverAddr)),
      name_(nameArg),
      connectionCallback_(defaultConnectionCallback),
      messageCallback_(defaultMessageCallback),
      retry_(false),
      connect_(true),
      nextConnId_(1) {
  connector_->setNewConnectionCallback(
      [this](int sockfd) { newConnection(sockfd); });
  // FIXME setConnectFailedCallback
  LOG(INFO) << "TcpClient::TcpClient[" << name_ << "] - connector "
            << get_pointer(connector_);
}

TcpClient::~TcpClient() {
  LOG(INFO) << "TcpClient::~TcpClient[" << name_ << "] - connector "
            << get_pointer(connector_);
  TcpConnectionPtr conn;
  bool unique = false;
  {
    std::unique_lock<std::mutex> lock(mutex_);
    unique = connection_.unique();
    conn = connection_;
  }
  if (conn) {
    assert(loop_ == conn->getLoop());
    // FIXME: not 100% safe, if we are in different thread
    CloseCallback cb = [loop = loop_](const TcpConnectionPtr &connection) {
      detail::removeConnection(loop, connection);
    };
    loop_->runInLoop([conn, cb]() { conn->setCloseCallback(cb); });
    if (unique) {
      conn->forceClose();
    }
  } else {
    connector_->stop();
    // FIXME: HACK
    // loop_->runAfter(1, std::bind(&detail::removeConnector, connector_));
  }

  std::vector<std::coroutine_handle<>> waiters;
  {
    std::scoped_lock lock(connectAwaiterMutex_);
    while (!connectWaiters_.empty()) {
      connectedQueue_.push_back(nullptr);
      waiters.push_back(connectWaiters_.front());
      connectWaiters_.pop_front();
    }
  }
  for (auto handle : waiters) {
    loop_->resumeInLoop(handle);
  }
}

Task<TcpConnectionPtr> TcpClient::connectAsync() {
  co_await loop_->schedule();
  connect();
  co_return co_await asyncConnect();
}

void TcpClient::connect() {
  // FIXME: check state
  LOG(INFO) << "TcpClient::connect[" << name_ << "] - connecting to "
            << connector_->serverAddress().toIpPort();
  connect_ = true;
  connector_->start();
}

void TcpClient::disconnect() {
  connect_ = false;

  {
    std::unique_lock<std::mutex> lock(mutex_);
    if (connection_) {
      connection_->shutdown();
    }
  }
}

void TcpClient::stop() {
  connect_ = false;
  connector_->stop();
}

void TcpClient::newConnection(int sockfd) {
  loop_->assertInLoopThread();
  InetAddress peerAddr(sockets::getPeerAddr(sockfd));
  char buf[32];
  snprintf(buf, sizeof buf, ":%s#%d", peerAddr.toIpPort().c_str(), nextConnId_);
  ++nextConnId_;
  std::string connName = name_ + buf;

  InetAddress localAddr(sockets::getLocalAddr(sockfd));
  // FIXME poll with zero timeout to double confirm the new connection
  // FIXME use make_shared if necessary
  auto conn =
      std::make_shared<TcpConnection>(loop_, connName, sockfd, localAddr,
                                      peerAddr);

  conn->setConnectionCallback(connectionCallback_);
  conn->setMessageCallback(messageCallback_);
  conn->setWriteCompleteCallback(writeCompleteCallback_);
  conn->setCloseCallback([this](const TcpConnectionPtr &connection) {
    removeConnection(connection);
  });
  {
    std::unique_lock<std::mutex> lock(mutex_);
    connection_ = conn;
  }
  publishConnected(conn);
  conn->connectEstablished();
}

void TcpClient::removeConnection(const TcpConnectionPtr &conn) {
  loop_->assertInLoopThread();
  assert(loop_ == conn->getLoop());

  {
    std::unique_lock<std::mutex> lock(mutex_);
    assert(connection_ == conn);
    connection_.reset();
  }

  loop_->queueInLoop([conn]() { conn->connectDestroyed(); });
  if (retry_ && connect_) {
    LOG(INFO) << "TcpClient::connect[" << name_ << "] - Reconnecting to "
              << connector_->serverAddress().toIpPort();
    connector_->restart();
  }
}

bool TcpClient::ConnectAwaiter::await_ready() {
  return client_->tryPopConnected(readyConnection_);
}

void TcpClient::ConnectAwaiter::await_suspend(
    std::coroutine_handle<> handle) {
  client_->enqueueConnectWaiter(handle);
}

TcpConnectionPtr TcpClient::ConnectAwaiter::await_resume() {
  if (readyConnection_) {
    return std::move(readyConnection_);
  }
  TcpConnectionPtr conn;
  client_->tryPopConnected(conn);
  return conn;
}

bool TcpClient::tryPopConnected(TcpConnectionPtr &conn) {
  {
    std::scoped_lock lock(connectAwaiterMutex_);
    if (!connectedQueue_.empty()) {
      conn = std::move(connectedQueue_.front());
      connectedQueue_.pop_front();
      return true;
    }
  }

  {
    std::scoped_lock lock(mutex_);
    if (connection_ && connection_->connected()) {
      conn = connection_;
      return true;
    }
  }

  return false;
}

void TcpClient::enqueueConnectWaiter(std::coroutine_handle<> handle) {
  bool hasConnection = false;
  {
    std::scoped_lock lock(connectAwaiterMutex_);
    if (connectedQueue_.empty()) {
      connectWaiters_.push_back(handle);
    } else {
      hasConnection = true;
    }
  }
  if (hasConnection) {
    loop_->resumeInLoop(handle);
  }
}

void TcpClient::publishConnected(const TcpConnectionPtr &conn) {
  std::coroutine_handle<> waiter;
  {
    std::scoped_lock lock(connectAwaiterMutex_);
    connectedQueue_.push_back(conn);
    if (!connectWaiters_.empty()) {
      waiter = connectWaiters_.front();
      connectWaiters_.pop_front();
    }
  }
  if (waiter) {
    loop_->resumeInLoop(waiter);
  }
}
