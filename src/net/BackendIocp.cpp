#include "LoopCore.h"

// placeholder until the IOCP backend is written
namespace solar::net::internal {
std::unique_ptr<Backend> MakeIocpBackend(Loop::Impl&) { return nullptr; }
}  // namespace solar::net::internal
