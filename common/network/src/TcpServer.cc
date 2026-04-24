#include "network/TcpServer.h"

#include <stdio.h>  // snprintf

#include <cassert>

#include <glog/logging.h>

#include "network/Acceptor.h"
#include "network/EventLoop.h"
#include "network/EventLoopThreadPool.h"
#include "network/SocketsOps.h"

using namespace network;

TcpServer::TcpServer(EventLoop *loop, const InetAddress &listenAddr,
                     const std::string &nameArg, Option option)
    : loop_(CHECK_NOTNULL(loop)),
      ipPort_(listenAddr.toIpPort()),
      name_(nameArg),
      acceptor_(new Acceptor(loop, listenAddr, option == kReusePort)),
      threadPool_(new EventLoopThreadPool(loop, name_)),
      connectionCallback_(defaultConnectionCallback),
      messageCallback_(defaultMessageCallback),
      started_(false),
      nextConnId_(1) {
  acceptor_->setNewConnectionCallback(
      [this](int sockfd, const InetAddress &peerAddr) {
        newConnection(sockfd, peerAddr);
      });
}

TcpServer::~TcpServer() {
  loop_->assertInLoopThread();
  LOG(INFO) << "TcpServer::~TcpServer [" << name_ << "] destructing";
  started_ = false;
  for (auto &item : connections_) {
    TcpConnectionPtr conn(item.second);
    item.second.reset();
    conn->getLoop()->runInLoop([conn]() { conn->connectDestroyed(); });
  }
}

void TcpServer::setThreadNum(int numThreads) {
  assert(0 <= numThreads);
  threadPool_->setThreadNum(numThreads);
}

void TcpServer::start() {
  if (started_.exchange(true)) {
    return;
  }
  threadPool_->start(threadInitCallback_);

  assert(!acceptor_->listening());

  loop_->runInLoop([acceptor = get_pointer(acceptor_)]() { acceptor->listen(); });
}

void TcpServer::newConnection(int sockfd, const InetAddress &peerAddr) {
  loop_->assertInLoopThread();
  EventLoop *ioLoop = threadPool_->getNextLoop();
  char buf[64];
  snprintf(buf, sizeof buf, "-%s#%d", ipPort_.c_str(), nextConnId_);
  ++nextConnId_;
  std::string connName = name_ + buf;

  LOG(INFO) << "TcpServer::newConnection [" << name_ << "] - new connection ["
            << connName << "] from " << peerAddr.toIpPort();
  InetAddress localAddr(sockets::getLocalAddr(sockfd));
  // FIXME poll with zero timeout to double confirm the new connection
  // FIXME use make_shared if necessary
  auto conn =
      std::make_shared<TcpConnection>(ioLoop, connName, sockfd, localAddr,
                                      peerAddr);
  connections_[connName] = conn;
  conn->setConnectionCallback(connectionCallback_);
  conn->setMessageCallback(messageCallback_);
  conn->setWriteCompleteCallback(writeCompleteCallback_);
  conn->setCloseCallback([this](const TcpConnectionPtr &connection) {
    removeConnection(connection);
  });  // FIXME: unsafe
  ioLoop->runInLoop([conn]() { conn->connectEstablished(); });
}

void TcpServer::removeConnection(const TcpConnectionPtr &conn) {
  // FIXME: unsafe
  loop_->runInLoop([this, conn]() { removeConnectionInLoop(conn); });
}

void TcpServer::removeConnectionInLoop(const TcpConnectionPtr &conn) {
  loop_->assertInLoopThread();
  LOG(INFO) << "TcpServer::removeConnectionInLoop [" << name_
            << "] - connection " << conn->name();
  size_t n = connections_.erase(conn->name());
  (void)n;
  assert(n == 1);
  EventLoop *ioLoop = conn->getLoop();
  ioLoop->queueInLoop([conn]() { conn->connectDestroyed(); });
}
