// The sandbox of the page's process: what it has it is given, and the rest is not there. Each check is made in a process of its own that enters the sandbox and then tries something: a
// thing that is to be refused ends the process (seccomp) or fails (files), a thing that is to work does.

#include <fcntl.h>
#include <spawn.h>
#include <linux/io_uring.h>
#include <netinet/in.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <string>
#include <thread>

#include "solar/gui/FrameBuffer.h"
#include "solar/gui/Sandbox.h"

extern char** environ;

using namespace solar::gui;

namespace {

int total = 0, failed = 0;
void Check(const std::string& name, bool ok, const std::string& detail = "") {
  ++total;
  if (ok) return;
  ++failed;
  std::printf("FAIL %s %s\n", name.c_str(), detail.c_str());
}

enum class Outcome { Worked, Killed, Failed };

// Runs `probe` in a process of its own, inside the sandbox. Worked: it returned 0. Failed: it returned something else. Killed: the system call it made was not allowed.
Outcome Run(const SandboxPolicy& policy, SandboxMode mode, const std::function<int()>& probe, std::string* why = nullptr) {
  std::fflush(stdout);
  const pid_t child = ::fork();
  if (child == 0) {
    const SandboxResult result = EnterSandbox(policy, mode);
    if (mode != SandboxMode::Off && !result.seccomp) _exit(90);
    _exit(probe());
  }
  int status = 0;
  ::waitpid(child, &status, 0);
  if (WIFSIGNALED(status)) {
    if (why) *why = "signal " + std::to_string(WTERMSIG(status));
    return WTERMSIG(status) == SIGSYS ? Outcome::Killed : Outcome::Failed;
  }
  if (why) *why = "exit " + std::to_string(WEXITSTATUS(status));
  return WEXITSTATUS(status) == 0 ? Outcome::Worked : Outcome::Failed;
}

std::string ReadWhole(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

}  // namespace

int main() {
  const std::string dir = "/tmp/solar-sandbox-test";
  ::mkdir(dir.c_str(), 0700);
  ::mkdir((dir + "/read").c_str(), 0700);
  ::mkdir((dir + "/write").c_str(), 0700);
  ::mkdir((dir + "/other").c_str(), 0700);
  { std::ofstream(dir + "/read/allowed.txt") << "allowed"; }
  { std::ofstream(dir + "/other/secret.txt") << "secret"; }
  SandboxPolicy policy;
  policy.readable = {dir + "/read", "/proc/self"};
  policy.writable = {dir + "/write"};
  int pair[2];
  ::socketpair(AF_UNIX, SOCK_STREAM, 0, pair);
  std::string why;

  // What is given works.
  Check("a file that may be read is read", Run(policy, SandboxMode::On, [&] { return ReadWhole(dir + "/read/allowed.txt") == "allowed" ? 0 : 1; }, &why) == Outcome::Worked, why);
  Check("a file in a directory that may be written is written and read", Run(policy, SandboxMode::On, [&] {
    { std::ofstream(dir + "/write/made.txt") << "made"; }
    return ReadWhole(dir + "/write/made.txt") == "made" && ::unlink((dir + "/write/made.txt").c_str()) == 0 ? 0 : 1;
  }, &why) == Outcome::Worked, why);
  Check("a thread can be made", Run(policy, SandboxMode::On, [&] {
    int value = 0;
    std::thread t([&] { value = 7; });
    t.join();
    return value == 7 ? 0 : 1;
  }, &why) == Outcome::Worked, why);
  Check("a connection that was given carries messages and a descriptor", Run(policy, SandboxMode::On, [&] {
    SharedMemory memory;
    if (!memory.Create(4096)) return 1;
    std::memcpy(memory.data(), "frame", 6);
    char byte = 'x';
    iovec vector{&byte, 1};
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))] = {};
    msghdr message{};
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    cmsghdr* header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(int));
    const int fd = memory.fd();
    std::memcpy(CMSG_DATA(header), &fd, sizeof(int));
    if (::sendmsg(pair[0], &message, 0) != 1) return 2;
    char got = 0;
    iovec in{&got, 1};
    alignas(cmsghdr) char inControl[CMSG_SPACE(sizeof(int))] = {};
    msghdr inMessage{};
    inMessage.msg_iov = &in;
    inMessage.msg_iovlen = 1;
    inMessage.msg_control = inControl;
    inMessage.msg_controllen = sizeof(inControl);
    return ::recvmsg(pair[1], &inMessage, 0) == 1 && got == 'x' ? 0 : 3;
  }, &why) == Outcome::Worked, why);
  Check("memory can be made, mapped and made executable (the engine's code)", Run(policy, SandboxMode::On, [&] {
    void* m = ::mmap(nullptr, 65536, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return m != MAP_FAILED && ::mprotect(m, 65536, PROT_READ | PROT_EXEC) == 0 ? 0 : 1;
  }, &why) == Outcome::Worked, why);

  // What is not given is not there: files.
  Check("a file that is not in the policy cannot be opened", Run(policy, SandboxMode::On, [&] { return ::open("/etc/passwd", O_RDONLY) < 0 && errno == EACCES ? 0 : 1; }, &why) == Outcome::Worked, why);
  Check("nor one next to those that may be", Run(policy, SandboxMode::On, [&] { return ::open((dir + "/other/secret.txt").c_str(), O_RDONLY) < 0 && errno == EACCES ? 0 : 1; }, &why) == Outcome::Worked, why);
  Check("a directory that may only be read cannot be written to", Run(policy, SandboxMode::On, [&] { return ::open((dir + "/read/new.txt").c_str(), O_WRONLY | O_CREAT, 0600) < 0 ? 0 : 1; }, &why) == Outcome::Worked, why);
  Check("a file that may be read cannot be removed", Run(policy, SandboxMode::On, [&] { return ::unlink((dir + "/read/allowed.txt").c_str()) != 0 ? 0 : 1; }, &why) == Outcome::Worked, why);
  Check("a directory cannot be made outside the writable one", Run(policy, SandboxMode::On, [&] { return ::mkdir((dir + "/other/new").c_str(), 0700) != 0 ? 0 : 1; }, &why) == Outcome::Worked, why);

  // What is not given is not there: the system calls.
  const auto killed = [&](const std::string& name, const std::function<int()>& probe) {
    std::string reason;
    Check(name, Run(policy, SandboxMode::On, probe, &reason) == Outcome::Killed, reason);
  };
  killed("a socket of the network ends the process", [] { return ::socket(AF_INET, SOCK_STREAM, 0) >= 0 ? 1 : 0; });
  killed("so does a local one that is new", [] { return ::socket(AF_UNIX, SOCK_STREAM, 0) >= 0 ? 1 : 0; });
  killed("starting another program", [] { char* const args[] = {const_cast<char*>("true"), nullptr}; ::execv("/bin/true", args); return 0; });
  killed("making a process", [] { return ::fork() >= 0 ? 1 : 0; });
  killed("looking into another process (ptrace)", [] { return static_cast<int>(::ptrace(PTRACE_TRACEME, 0, 0, 0)) >= 0 ? 1 : 0; });
  killed("mounting", [] { return ::mount("none", "/tmp", "tmpfs", 0, nullptr) == 0 ? 1 : 0; });
  killed("io_uring", [] { io_uring_params params{}; return ::syscall(SYS_io_uring_setup, 4, &params) >= 0 ? 1 : 0; });
  killed("a new namespace", [] { return ::unshare(CLONE_NEWNS) == 0 ? 1 : 0; });
  killed("a signal to another process", [] { return ::kill(1, 0) == 0 ? 1 : 0; });
  killed("a process made with namespaces by clone3", [] {
    // (clone3 is told it is not there, and the library falls back: a program that asks for it by hand and goes on gets nothing either.)
    return ::syscall(SYS_clone, CLONE_NEWUSER | SIGCHLD, 0, 0, 0, 0) >= 0 ? 1 : 0;
  });
  killed("a perf event", [] { return ::syscall(SYS_perf_event_open, nullptr, 0, -1, -1, 0) >= 0 ? 1 : 0; });
  killed("reading another process's memory", [] { return ::syscall(SYS_process_vm_readv, 1, nullptr, 0, nullptr, 0, 0) >= 0 ? 1 : 0; });
  killed("loading a BPF program", [] { return ::syscall(SYS_bpf, 0, nullptr, 0) >= 0 ? 1 : 0; });
  killed("a prctl that is not for threads", [] { return ::prctl(PR_SET_SECCOMP, 0, 0, 0, 0) == 0 ? 1 : 0; });

  // A program that is allowed may be started, and no other.
  {
    SandboxPolicy withProgram = policy;
    withProgram.executable = {"/usr/bin/true", "/usr/lib/ld-linux-x86-64.so.2", "/usr/lib64/ld-linux-x86-64.so.2"};
    withProgram.readable.push_back("/usr/lib");
    Check("a program that is allowed is started (and its end waited for)", Run(withProgram, SandboxMode::On, [&] {
      pid_t child = -1;
      char arg0[] = "true";
      char* arguments[] = {arg0, nullptr};
      if (::posix_spawn(&child, "/usr/bin/true", nullptr, nullptr, arguments, environ) != 0) return 1;
      int status = 0;
      return ::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : 2;
    }, &why) == Outcome::Worked, why);
    Check("another is not (the file is not there to be executed)", Run(withProgram, SandboxMode::On, [&] {
      pid_t child = -1;
      char arg0[] = "false";
      char* arguments[] = {arg0, nullptr};
      const int result = ::posix_spawn(&child, "/usr/bin/false", nullptr, nullptr, arguments, environ);
      if (result != 0) return 0;
      int status = 0;
      ::waitpid(child, &status, 0);
      // (A spawn is a child that did not execute: it ends with 127.)
      return WIFEXITED(status) && WEXITSTATUS(status) == 127 ? 0 : 1;
    }, &why) == Outcome::Worked, why);
    Check("and without any, a process is not made", Run(policy, SandboxMode::On, [&] {
      pid_t child = -1;
      char arg0[] = "true";
      char* arguments[] = {arg0, nullptr};
      return ::posix_spawn(&child, "/usr/bin/true", nullptr, nullptr, arguments, environ) == 0 ? 1 : 0;
    }, &why) == Outcome::Killed, why);
  }

  // The rest of what the process is: no capabilities, the user the same, no network namespace shared.
  {
    const std::string parentNet = [] { char b[64] = {}; ssize_t n = ::readlink("/proc/self/ns/net", b, sizeof b - 1); return std::string(b, n > 0 ? n : 0); }();
    Check("the process is in a network namespace of its own", Run(policy, SandboxMode::On, [&] {
      char b[64] = {};
      const ssize_t n = ::readlink("/proc/self/ns/net", b, sizeof b - 1);
      return n > 0 && std::string(b, n) != parentNet ? 0 : 1;
    }, &why) == Outcome::Worked, why);
    Check("it has no capabilities", Run(policy, SandboxMode::On, [&] {
      const std::string status = ReadWhole("/proc/self/status");
      const size_t at = status.find("CapEff:\t");
      return at != std::string::npos && status.compare(at + 8, 16, "0000000000000000") == 0 ? 0 : 1;
    }, &why) == Outcome::Worked, why);
    const uid_t uid = ::getuid();
    Check("it is the same user", Run(policy, SandboxMode::On, [&] { return ::getuid() == uid ? 0 : 1; }, &why) == Outcome::Worked, why);
  }

  // Report: what is not allowed fails with "not implemented" and the process goes on.
  Check("in report mode a call that is not allowed fails and the process goes on", Run(policy, SandboxMode::Report, [] { return ::socket(AF_INET, SOCK_STREAM, 0) < 0 && errno == ENOSYS ? 0 : 1; }, &why) == Outcome::Worked, why);
  Check("with the sandbox off nothing is refused", Run(policy, SandboxMode::Off, [&] { const int fd = ::open("/etc/passwd", O_RDONLY); if (fd < 0) return 1; ::close(fd); const int s = ::socket(AF_INET, SOCK_STREAM, 0); return s >= 0 ? 0 : 2; }, &why) == Outcome::Worked, why);

  std::printf("sandbox: %d/%d passed\n", total - failed, total);
  return failed == 0 ? 0 : 1;
}
