#pragma once

#include <unordered_map>
#include <vector>

#include "EventLoop.h"
struct epoll_event;
namespace network {
class Channel;
///
/// IO Multiplexing with epoll(4).
///
class Poller {
 public:
  using ChannelList = std::vector<Channel *>;
  Poller(EventLoop *loop);
  ~Poller();

  void poll(int timeoutMs, ChannelList *activeChannels);
  void updateChannel(Channel *channel);
  void removeChannel(Channel *channel);

  void assertInLoopThread() const { ownerLoop_->assertInLoopThread(); }
  bool hasChannel(Channel *channel) const;

 private:
  EventLoop *ownerLoop_;
  static const int kInitEventListSize = 16;

  static const char *operationToString(int op);

  void fillActiveChannels(int numEvents, ChannelList *activeChannels) const;
  void update(int operation, Channel *channel);

  using EventList = std::vector<struct epoll_event>;

  int epollfd_;
  EventList events_;

  using ChannelMap = std::unordered_map<int, Channel *>;
  ChannelMap channels_;
};

}  // namespace network
