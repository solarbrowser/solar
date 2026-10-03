#include "ConnectRace.h"

#include "Socket.h"

namespace solar::net::internal {

namespace {

int Index(AddressRace::Timer which) { return which == AddressRace::Timer::ResolutionDelay ? 0 : 1; }
int Index(SocketAddress::Family family) { return family == SocketAddress::Family::IPv6 ? 0 : 1; }

}  // namespace

// One connection being tried. It is the connection's handler until the race is decided: a loser
// closes it and it deletes itself once the loop says the connection is over, and the winner is
// handed on to the owner, which makes it the connection's handler instead.
struct ConnectRace::Attempt : ConnectionHandler {
  Attempt(std::shared_ptr<Shared> s, AddressRace::AttemptId i) : shared(std::move(s)), id(i) {}

  void OnConnected() override {
    ConnectRace* race = shared->race;
    if (!race || abandoned) {
      connection->Close();
      return;
    }
    race->race_.AttemptSucceeded(id);
    // The winner's handler was replaced while that call ran, so the loop will not come back here.
    if (handedOff) delete this;
  }

  void OnData(std::span<const uint8_t>) override {}  // receiving starts only once it is the winner's

  void OnClosed(int error) override {
    if (ConnectRace* race = shared->race) {
      race->attempts_.erase(id);
      if (!abandoned) race->race_.AttemptFailed(id, ErrorMessage(error));
    }
    delete this;
  }

  std::shared_ptr<Shared> shared;
  AddressRace::AttemptId id;
  Connection* connection = nullptr;
  bool abandoned = false;
  bool handedOff = false;
};

ConnectRace::ConnectRace(Loop& loop, Resolver& resolver, std::string host, uint16_t port, AddressRace::Config config, OnConnected connected,
                         OnFailed failed)
    : loop_(loop),
      resolver_(resolver),
      host_(std::move(host)),
      port_(port),
      connected_(std::move(connected)),
      failed_(std::move(failed)),
      shared_(std::make_shared<Shared>(Shared{this})),
      race_(*this, config) {}

ConnectRace::~ConnectRace() {
  shared_->race = nullptr;  // what is still out finds nobody to tell
  race_.Abandon();
  for (Loop::TimerId& timer : timers_) {
    if (timer != 0) loop_.CancelTimer(timer);
  }
  for (Resolver::RequestId id : lookups_) {
    if (id != 0) resolver_.Cancel(id);
  }
}

void ConnectRace::Start() {
  for (SocketAddress::Family family : {SocketAddress::Family::IPv6, SocketAddress::Family::IPv4}) {
    const int slot = Index(family);
    lookups_[slot] = resolver_.Resolve(host_, port_, family, [shared = std::weak_ptr<Shared>(shared_), family, slot](ResolveResult result) {
      std::shared_ptr<Shared> alive = shared.lock();
      if (!alive || !alive->race) return;
      alive->race->lookups_[slot] = 0;
      alive->race->race_.Answered(family, std::move(result));
    });
  }
}

AddressRace::AttemptId ConnectRace::StartAttempt(const SocketAddress& address) {
  const AddressRace::AttemptId id = ++nextAttempt_;
  auto* attempt = new Attempt(shared_, id);
  attempts_[id] = attempt;
  attempt->connection = loop_.Connect(address, attempt);
  return id;
}

void ConnectRace::CancelAttempt(AddressRace::AttemptId id) {
  auto it = attempts_.find(id);
  if (it == attempts_.end()) return;
  it->second->abandoned = true;
  it->second->connection->Close();
}

void ConnectRace::StartTimer(AddressRace::Timer which, std::chrono::milliseconds delay) {
  StopTimer(which);
  const int slot = Index(which);
  timers_[slot] = loop_.PostDelayed(delay, [shared = std::weak_ptr<Shared>(shared_), which, slot] {
    std::shared_ptr<Shared> alive = shared.lock();
    if (!alive || !alive->race) return;
    alive->race->timers_[slot] = 0;
    alive->race->race_.TimerFired(which);
  });
}

void ConnectRace::StopTimer(AddressRace::Timer which) {
  Loop::TimerId& timer = timers_[Index(which)];
  if (timer == 0) return;
  loop_.CancelTimer(timer);
  timer = 0;
}

void ConnectRace::Succeeded(AddressRace::AttemptId id) {
  // A lookup that has not answered is no longer wanted.
  for (Resolver::RequestId& lookup : lookups_) {
    if (lookup != 0) resolver_.Cancel(lookup);
    lookup = 0;
  }
  auto it = attempts_.find(id);
  Attempt* winner = it->second;
  attempts_.erase(it);
  winner->handedOff = true;
  connected_(winner->connection);
}

void ConnectRace::Failed(const std::string& message) { failed_(message); }

}  // namespace solar::net::internal
