#include <atomic>
#include <coroutine>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

#include "all.h"
#include "network/Task.h"
#include "session.h"

std::atomic<int> counter_port(8000);
network::EventLoop loop;
std::unique_ptr<network::TcpServer> server;

namespace {

struct DetachedTask {
  struct promise_type {
    DetachedTask get_return_object() noexcept { return {}; }
    std::suspend_never initial_suspend() const noexcept { return {}; }
    std::suspend_never final_suspend() const noexcept { return {}; }
    void return_void() const noexcept {}
    void unhandled_exception() noexcept {
      try {
        throw;
      } catch (const std::exception &e) {
        std::cerr << "tcp detached coroutine exception: " << e.what()
                  << std::endl;
      } catch (...) {
        std::cerr << "tcp detached coroutine exception: unknown"
                  << std::endl;
      }
    }
  };
};

DetachedTask runDetached(network::Task<void> task) {
  co_await std::move(task);
}

network::Task<void> tcpSessionLoop(const network::TcpConnectionPtr &conn,
                                   const std::shared_ptr<TcpSession> &session) {
  try {
    while (conn->connected()) {
      auto message = co_await conn->receiveAsync();
      if (!message.has_value()) {
        break;
      }

      session->select_json_str(*message, [session](const std::string &data) {
        session->on_data(data);
      });
    }
  } catch (const std::exception &e) {
    std::cerr << "tcp session coroutine error: " << e.what() << std::endl;
  } catch (...) {
    std::cerr << "tcp session coroutine error: unknown" << std::endl;
  }

  session->stop();
  co_return;
}

void onConnection(const network::TcpConnectionPtr &conn) {
  if (!conn->connected()) {
    return;
  }

  auto session = std::make_shared<TcpSession>(conn);
  session->work(zmq_s_format, counter_port.fetch_add(1));
  if (counter_port > 65535) {
    counter_port.store(8000);
  }

  runDetached(tcpSessionLoop(conn, session));
}

}  // namespace

void tcp_work() {
  int listenport = 0;
  SAFE_READING(listenport, int, "config_tcp_server");
  network::InetAddress listenAddr(listenport);
  server = std::make_unique<network::TcpServer>(&loop, listenAddr, "ZMQBridge");

  server->setConnectionCallback(onConnection);
  server->setThreadNum(2);
  server->start();
  loop.loop();
}

void tcp_stop_work() {
  loop.quit();
  server.reset();
}
