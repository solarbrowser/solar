#pragma once

#include <future>
#include <memory>
#include <thread>

#include "solar/net/HttpClient.h"
#include "solar/net/NetworkThread.h"
#include "solar/net/RemoteNetwork.h"
#include "support/TestLoop.h"

namespace solar::test {

// The service side of a page whose network is in another thread of this process (as it is in another process): a loop that runs on a thread, the network on a thread of its own, and
// the end of the connection that reads what the page asks.
// (The origin the page may ask for is given by SetOrigin of the host.)
struct RemoteSide {
  std::unique_ptr<solar::net::Loop> loop = solar::test::MakeLoop();
  std::thread thread;
  std::unique_ptr<solar::net::ThreadedNetworkService> network;
  std::unique_ptr<solar::net::NetworkServiceHost> host;
  explicit RemoteSide(int fd) {
    network = std::make_unique<solar::net::ThreadedNetworkService>(*loop);
    std::promise<void> ready;
    thread = std::thread([this, &ready] {
      loop->BeginExternalWork();
      ready.set_value();
      loop->Run();
    });
    ready.get_future().wait();
    host = std::make_unique<solar::net::NetworkServiceHost>(*network, *loop, fd, "");
  }
  ~RemoteSide() {
    host.reset();
    // (What the host ends is ended by a task of the loop: it has gone through all that was put before this when this has run.)
    std::promise<void> flushed;
    loop->Post([&flushed] { flushed.set_value(); });
    flushed.get_future().wait();
    network.reset();
    loop->Post([this] {
      loop->EndExternalWork();
      loop->Stop();
    });
    thread.join();
  }
};

}  // namespace solar::test
