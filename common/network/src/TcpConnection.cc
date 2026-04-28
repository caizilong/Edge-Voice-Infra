#include "TcpConnection.h"

#include <errno.h>

#include <cassert>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <glog/logging.h>

#include "Channel.h"
#include "EventLoop.h"
#include "Socket.h"
#include "SocketsOps.h"

using namespace network;
namespace network {
void defaultConnectionCallback(const TcpConnectionPtr &conn) {
  LOG(INFO) << conn->localAddress().toIpPort() << " -> "
            << conn->peerAddress().toIpPort() << " is "
            << (conn->connected() ? "UP" : "DOWN");
  // do not call conn->forceClose(), because some users want to register message
  // callback only.
}

void defaultMessageCallback(const TcpConnectionPtr &, Buffer *buf) {
  buf->retrieveAll();
}

TcpConnection::TcpConnection(EventLoop *loop, const std::string &nameArg,
                             int sockfd, const InetAddress &localAddr,
                             const InetAddress &peerAddr)
    : loop_(CHECK_NOTNULL(loop)),
      name_(nameArg),
      state_(kConnecting),
      reading_(true),
      socket_(new Socket(sockfd)),
      channel_(new Channel(loop, sockfd)),
      localAddr_(localAddr),
      peerAddr_(peerAddr) {
  channel_->setReadCallback([this]() { handleRead(); });
  channel_->setWriteCallback([this]() { handleWrite(); });
  channel_->setCloseCallback([this]() { handleClose(); });
  channel_->setErrorCallback([this]() { handleError(); });
  LOG(INFO) << "TcpConnection::ctor[" << name_ << "] at " << this
            << " fd=" << sockfd;
  socket_->setKeepAlive(true);
}

TcpConnection::~TcpConnection() {
  LOG(INFO) << "TcpConnection::dtor[" << name_ << "] at " << this
            << " fd=" << channel_->fd() << " state=" << stateToString();
  assert(state_ == kDisconnected);
}

bool TcpConnection::getTcpInfo(struct tcp_info *tcpi) const {
  return socket_->getTcpInfo(tcpi);
}

std::string TcpConnection::getTcpInfoString() const {
  char buf[1024];
  buf[0] = '\0';
  socket_->getTcpInfoString(buf, sizeof buf);
  return buf;
}

// FIXME efficiency!!!
void TcpConnection::send(Buffer *buf) {
  if (buf == nullptr) {
    return;
  }
  if (state_ == kConnected) {
    if (loop_->isInLoopThread()) {
      sendInLoop(buf->peek(), buf->readableBytes());
      buf->retrieveAll();
    } else {
      std::string message = buf->retrieveAllAsString();
      auto self = shared_from_this();
      loop_->runInLoop([self, message = std::move(message)]() mutable {
        self->sendInLoop(message.data(), message.size());
      });
    }
  }
}

void TcpConnection::send(std::string_view message) {
  if (state_ != kConnected) {
    return;
  }
  if (loop_->isInLoopThread()) {
    sendInLoop(message.data(), message.size());
    return;
  }

  std::string copied(message);
  auto self = shared_from_this();
  loop_->runInLoop([self, copied = std::move(copied)]() mutable {
    self->sendInLoop(copied.data(), copied.size());
  });
}

Task<size_t> TcpConnection::sendAsync(std::string message) {
  co_await loop_->schedule();
  if (state_ != kConnected) {
    co_return 0;
  }
  sendInLoop(message.data(), message.size());
  co_return message.size();
}

Task<std::optional<std::string>> TcpConnection::receiveAsync() {
  co_await loop_->schedule();
  co_return co_await asyncRead();
}

bool TcpConnection::AsyncReadAwaiter::await_ready() {
  return conn_->tryPopAsyncRead(readyChunk_);
}

void TcpConnection::AsyncReadAwaiter::await_suspend(
    std::coroutine_handle<> handle) {
  conn_->enqueueAsyncReadWaiter(handle);
}

std::optional<std::string> TcpConnection::AsyncReadAwaiter::await_resume() {
  if (readyChunk_.has_value()) {
    return std::move(readyChunk_);
  }
  std::optional<std::string> chunk;
  conn_->tryPopAsyncRead(chunk);
  return chunk;
}

bool TcpConnection::tryPopAsyncRead(std::optional<std::string> &chunk) {
  std::scoped_lock lock(asyncReadMutex_);
  if (!asyncReadQueue_.empty()) {
    chunk = std::move(asyncReadQueue_.front());
    asyncReadQueue_.pop_front();
    return true;
  }
  if (asyncReadClosed_) {
    chunk.reset();
    return true;
  }
  return false;
}

void TcpConnection::enqueueAsyncReadWaiter(std::coroutine_handle<> handle) {
  bool hasData = false;
  {
    std::scoped_lock lock(asyncReadMutex_);
    if (!asyncReadQueue_.empty() || asyncReadClosed_) {
      hasData = true;
    } else {
      asyncReadWaiters_.push_back(handle);
    }
  }
  if (hasData) {
    loop_->resumeInLoop(handle);
  }
}

void TcpConnection::publishAsyncRead(std::string chunk) {
  std::coroutine_handle<> waiter;
  {
    std::scoped_lock lock(asyncReadMutex_);
    if (asyncReadClosed_) {
      return;
    }
    asyncReadQueue_.push_back(std::move(chunk));
    if (!asyncReadWaiters_.empty()) {
      waiter = asyncReadWaiters_.front();
      asyncReadWaiters_.pop_front();
    }
  }
  if (waiter) {
    loop_->resumeInLoop(waiter);
  }
}

void TcpConnection::publishAsyncClosed() {
  std::vector<std::coroutine_handle<>> waiters;
  {
    std::scoped_lock lock(asyncReadMutex_);
    if (asyncReadClosed_) {
      return;
    }
    asyncReadClosed_ = true;
    while (!asyncReadWaiters_.empty()) {
      asyncReadQueue_.push_back(std::nullopt);
      waiters.push_back(asyncReadWaiters_.front());
      asyncReadWaiters_.pop_front();
    }
  }
  for (auto handle : waiters) {
    loop_->resumeInLoop(handle);
  }
}

void TcpConnection::sendInLoop(const std::string &message) {
  sendInLoop(message.data(), message.size());
}

void TcpConnection::sendInLoop(const void *data, size_t len) {
  loop_->assertInLoopThread();
  ssize_t nwrote = 0;
  size_t remaining = len;
  bool faultError = false;
  if (state_ == kDisconnected) {
    LOG(INFO) << "disconnected, give up writing";
    return;
  }
  // if no thing in output queue, try writing directly
  if (!channel_->isWriting() && outputBuffer_.readableBytes() == 0) {
    nwrote = sockets::write(channel_->fd(), data, len);
    if (nwrote >= 0) {
      remaining = len - nwrote;
      if (remaining == 0 && writeCompleteCallback_) {
        auto callback = writeCompleteCallback_;
        auto self = shared_from_this();
        loop_->queueInLoop([callback, self]() { callback(self); });
      }
    } else  // nwrote < 0
    {
      nwrote = 0;
      if (errno != EWOULDBLOCK) {
        LOG(ERROR) << "TcpConnection::sendInLoop";
        if (errno == EPIPE || errno == ECONNRESET)  // FIXME: any others?
        {
          faultError = true;
        }
      }
    }
  }

  if (!faultError && remaining > 0) {
    outputBuffer_.append(static_cast<const char *>(data) + nwrote, remaining);
    if (!channel_->isWriting()) {
      channel_->enableWriting();
    }
  }
}

void TcpConnection::shutdown() {
  // FIXME: use compare and swap
  if (state_ == kConnected) {
    setState(kDisconnecting);
    auto self = shared_from_this();
    loop_->runInLoop([self]() { self->shutdownInLoop(); });
  }
}

void TcpConnection::shutdownInLoop() {
  loop_->assertInLoopThread();
  if (!channel_->isWriting()) {
    // we are not writing
    socket_->shutdownWrite();
  }
}

// void TcpConnection::shutdownAndForceCloseAfter(double seconds)
// {
//   // FIXME: use compare and swap
//   if (state_ == kConnected)
//   {
//     setState(kDisconnecting);
//     loop_->runInLoop(std::bind(&TcpConnection::shutdownAndForceCloseInLoop,
//     this, seconds));
//   }
// }

// void TcpConnection::shutdownAndForceCloseInLoop(double seconds)
// {
//   loop_->assertInLoopThread();
//   if (!channel_->isWriting())
//   {
//     // we are not writing
//     socket_->shutdownWrite();
//   }
//   loop_->runAfter(
//       seconds,
//       makeWeakCallback(shared_from_this(),
//                        &TcpConnection::forceCloseInLoop));
// }

void TcpConnection::forceClose() {
  // FIXME: use compare and swap
  if (state_ == kConnected || state_ == kDisconnecting) {
    setState(kDisconnecting);
    auto self = shared_from_this();
    loop_->queueInLoop([self]() { self->forceCloseInLoop(); });
  }
}

void TcpConnection::forceCloseWithDelay(double seconds) {
  if (state_ == kConnected || state_ == kDisconnecting) {
    setState(kDisconnecting);
    // loop_->runAfter(
    //     seconds,
    //     makeWeakCallback(shared_from_this(),
    //                      &TcpConnection::forceClose));  // not
    //                      forceCloseInLoop to avoid race condition
  }
}

void TcpConnection::forceCloseInLoop() {
  loop_->assertInLoopThread();
  if (state_ == kConnected || state_ == kDisconnecting) {
    // as if we received 0 byte in handleRead();
    handleClose();
  }
}

const char *TcpConnection::stateToString() const {
  switch (state_) {
    case kDisconnected:
      return "kDisconnected";
    case kConnecting:
      return "kConnecting";
    case kConnected:
      return "kConnected";
    case kDisconnecting:
      return "kDisconnecting";
    default:
      return "unknown state";
  }
}

void TcpConnection::setTcpNoDelay(bool on) { socket_->setTcpNoDelay(on); }

void TcpConnection::startRead() {
  auto self = shared_from_this();
  loop_->runInLoop([self]() { self->startReadInLoop(); });
}

void TcpConnection::startReadInLoop() {
  loop_->assertInLoopThread();
  if (!reading_ || !channel_->isReading()) {
    channel_->enableReading();
    reading_ = true;
  }
}

void TcpConnection::stopRead() {
  auto self = shared_from_this();
  loop_->runInLoop([self]() { self->stopReadInLoop(); });
}

void TcpConnection::stopReadInLoop() {
  loop_->assertInLoopThread();
  if (reading_ || channel_->isReading()) {
    channel_->disableReading();
    reading_ = false;
  }
}

void TcpConnection::connectEstablished() {
  loop_->assertInLoopThread();
  assert(state_ == kConnecting);
  setState(kConnected);
  // channel_->tie(shared_from_this());
  channel_->enableReading();

  connectionCallback_(shared_from_this());
}

void TcpConnection::connectDestroyed() {
  loop_->assertInLoopThread();
  if (state_ == kConnected) {
    setState(kDisconnected);
    channel_->disableAll();
    connectionCallback_(shared_from_this());
  }
  publishAsyncClosed();
  channel_->remove();
}

void TcpConnection::handleRead() {
  loop_->assertInLoopThread();
  int savedErrno = 0;
  const size_t readableBeforeRead = inputBuffer_.readableBytes();
  ssize_t n = inputBuffer_.readFd(channel_->fd(), &savedErrno);
  if (n > 0) {
    const size_t readableAfterRead = inputBuffer_.readableBytes();
    const size_t appended = readableAfterRead - readableBeforeRead;
    if (appended > 0) {
      publishAsyncRead(std::string(inputBuffer_.peek() + readableBeforeRead,
                                   appended));
    }
    messageCallback_(shared_from_this(), &inputBuffer_);
  } else if (n == 0) {
    handleClose();
  } else {
    errno = savedErrno;
    LOG(ERROR) << "TcpConnection::handleRead";
    handleError();
  }
}

void TcpConnection::handleWrite() {
  loop_->assertInLoopThread();
  if (channel_->isWriting()) {
    ssize_t n = sockets::write(channel_->fd(), outputBuffer_.peek(),
                               outputBuffer_.readableBytes());
    if (n > 0) {
      outputBuffer_.retrieve(n);
      if (outputBuffer_.readableBytes() == 0) {
        channel_->disableWriting();
        if (writeCompleteCallback_) {
          auto callback = writeCompleteCallback_;
          auto self = shared_from_this();
          loop_->queueInLoop([callback, self]() { callback(self); });
        }
        if (state_ == kDisconnecting) {
          shutdownInLoop();
        }
      }
    } else {
      LOG(ERROR) << "TcpConnection::handleWrite";
      // if (state_ == kDisconnecting)
      // {
      //   shutdownInLoop();
      // }
    }
  } else {
    LOG(INFO) << "Connection fd = " << channel_->fd()
              << " is down, no more writing";
  }
}

void TcpConnection::handleClose() {
  loop_->assertInLoopThread();
  LOG(INFO) << "fd = " << channel_->fd() << " state = " << stateToString();
  assert(state_ == kConnected || state_ == kDisconnecting);
  // we don't close fd, leave it to dtor, so we can find leaks easily.
  setState(kDisconnected);
  channel_->disableAll();
  publishAsyncClosed();

  TcpConnectionPtr guardThis(shared_from_this());
  connectionCallback_(guardThis);
  // must be the last line
  closeCallback_(guardThis);
}

void TcpConnection::handleError() {
  int err = sockets::getSocketError(channel_->fd());
  LOG(ERROR) << "TcpConnection::handleError [" << name_
             << "] - SO_ERROR = " << err;
}
}  // namespace network
