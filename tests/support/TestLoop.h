#pragma once

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include "solar/net/Loop.h"

namespace solar::test {

// A loop on the backend named by SOLAR_LOOP_BACKEND ("uring" or "readiness"), or the platform's
// best if it is unset, so one set of tests can be run against each.
inline std::unique_ptr<solar::net::Loop> MakeLoop() {
  solar::net::LoopBackend backend = solar::net::LoopBackend::Automatic;
  if (const char* choice = std::getenv("SOLAR_LOOP_BACKEND")) {
    const std::string name = choice;
    if (name == "uring") backend = solar::net::LoopBackend::IoUring;
    if (name == "readiness") backend = solar::net::LoopBackend::Readiness;
  }
  auto loop = solar::net::Loop::Create(backend);
  if (!loop) {
    std::printf("cannot create the event loop for this platform\n");
    std::exit(2);
  }
  static bool announced = false;
  if (!announced) {
    announced = true;
    std::printf("[event loop: %s]\n", loop->backend_name());
  }
  return loop;
}

}  // namespace solar::test
