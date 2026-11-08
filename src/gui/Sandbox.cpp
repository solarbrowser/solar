/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "solar/gui/Sandbox.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#if defined(__linux__) && defined(__x86_64__)

#include <errno.h>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/capability.h>
#include <linux/filter.h>
#include <linux/landlock.h>
#include <linux/seccomp.h>
#include <sched.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <ucontext.h>
#include <unistd.h>

#include <algorithm>
#include <string>
#include <vector>

namespace solar::gui {

namespace {

// ---- Namespaces ----

bool WriteFile(const char* path, const std::string& text) {
  const int fd = ::open(path, O_WRONLY | O_CLOEXEC);
  if (fd < 0) return false;
  const ssize_t n = ::write(fd, text.data(), text.size());
  ::close(fd);
  return n == static_cast<ssize_t>(text.size());
}

// New user, network, IPC and host name namespaces, the user made the same one inside (so that the files it may open are the same), and every capability dropped.
bool EnterNamespaces(std::string& problems) {
  const uid_t uid = ::geteuid();
  const gid_t gid = ::getegid();
  if (::unshare(CLONE_NEWUSER | CLONE_NEWNET | CLONE_NEWIPC | CLONE_NEWUTS) != 0) {
    problems += std::string("namespaces: ") + std::strerror(errno) + "; ";
    return false;
  }
  bool ok = WriteFile("/proc/self/setgroups", "deny") && WriteFile("/proc/self/uid_map", std::to_string(uid) + " " + std::to_string(uid) + " 1\n") &&
            WriteFile("/proc/self/gid_map", std::to_string(gid) + " " + std::to_string(gid) + " 1\n");
  if (!ok) problems += "namespaces: the user could not be mapped; ";
  // The capabilities that the process has inside the new user namespace are let go of for good (and cannot be got back: no new privileges is set below).
  __user_cap_header_struct header{};
  header.version = _LINUX_CAPABILITY_VERSION_3;
  header.pid = 0;
  __user_cap_data_struct data[2] = {};
  if (::syscall(SYS_capset, &header, data) != 0) {
    problems += "namespaces: capabilities not dropped; ";
    ok = false;
  }
  for (int cap = 0; cap <= 63; ++cap) {
    if (::prctl(PR_CAPBSET_DROP, cap, 0, 0, 0) != 0 && errno == EINVAL) break;
  }
  return ok;
}

// ---- Landlock ----

int LandlockCreate(const landlock_ruleset_attr* attr, size_t size, uint32_t flags) { return static_cast<int>(::syscall(SYS_landlock_create_ruleset, attr, size, flags)); }
int LandlockAdd(int ruleset, landlock_rule_type type, const void* attr) { return static_cast<int>(::syscall(SYS_landlock_add_rule, ruleset, type, attr, 0)); }
int LandlockRestrict(int ruleset) { return static_cast<int>(::syscall(SYS_landlock_restrict_self, ruleset, 0)); }

bool EnterLandlock(const SandboxPolicy& policy, std::string& problems) {
  const int abi = LandlockCreate(nullptr, 0, LANDLOCK_CREATE_RULESET_VERSION);
  if (abi < 1) {
    problems += "landlock: not in this kernel; ";
    return false;
  }
  // Every right the kernel knows is handled (a right that is handled and not given by a rule is refused).
  uint64_t fsAll = LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR | LANDLOCK_ACCESS_FS_REMOVE_DIR |
                   LANDLOCK_ACCESS_FS_REMOVE_FILE | LANDLOCK_ACCESS_FS_MAKE_CHAR | LANDLOCK_ACCESS_FS_MAKE_DIR | LANDLOCK_ACCESS_FS_MAKE_REG | LANDLOCK_ACCESS_FS_MAKE_SOCK |
                   LANDLOCK_ACCESS_FS_MAKE_FIFO | LANDLOCK_ACCESS_FS_MAKE_BLOCK | LANDLOCK_ACCESS_FS_MAKE_SYM;
  if (abi >= 2) fsAll |= LANDLOCK_ACCESS_FS_REFER;
  if (abi >= 3) fsAll |= LANDLOCK_ACCESS_FS_TRUNCATE;
  if (abi >= 5) fsAll |= LANDLOCK_ACCESS_FS_IOCTL_DEV;
  landlock_ruleset_attr attr{};
  attr.handled_access_fs = fsAll;
  if (abi >= 4) attr.handled_access_net = LANDLOCK_ACCESS_NET_BIND_TCP | LANDLOCK_ACCESS_NET_CONNECT_TCP;
  if (abi >= 6) attr.scoped = LANDLOCK_SCOPE_ABSTRACT_UNIX_SOCKET | LANDLOCK_SCOPE_SIGNAL;
  const int ruleset = LandlockCreate(&attr, sizeof(attr), 0);
  if (ruleset < 0) {
    problems += std::string("landlock: ") + std::strerror(errno) + "; ";
    return false;
  }
  const uint64_t readRights = LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR;
  const auto add = [&](const std::string& path, uint64_t rights) {
    const int fd = ::open(path.c_str(), O_PATH | O_CLOEXEC);
    if (fd < 0) return;  // (what is not there is not allowed either)
    // A rule for a file may carry only the rights that are of files.
    struct stat info;
    uint64_t allowed = rights & fsAll;
    if (::fstat(fd, &info) == 0 && !S_ISDIR(info.st_mode)) allowed &= LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_READ_FILE | (abi >= 3 ? LANDLOCK_ACCESS_FS_TRUNCATE : 0) | (abi >= 5 ? LANDLOCK_ACCESS_FS_IOCTL_DEV : 0);
    landlock_path_beneath_attr rule{};
    rule.allowed_access = allowed;
    rule.parent_fd = fd;
    if (LandlockAdd(ruleset, LANDLOCK_RULE_PATH_BENEATH, &rule) != 0) problems += "landlock: no rule for " + path + "; ";
    ::close(fd);
  };
  for (const std::string& path : policy.readable) add(path, readRights);
  for (const std::string& path : policy.executable) add(path, readRights | LANDLOCK_ACCESS_FS_EXECUTE);
  for (const std::string& path : policy.writable) add(path, fsAll & ~LANDLOCK_ACCESS_FS_EXECUTE);
  const bool ok = LandlockRestrict(ruleset) == 0;
  if (!ok) problems += std::string("landlock: ") + std::strerror(errno) + "; ";
  ::close(ruleset);
  return ok;
}

// ---- seccomp ----

constexpr int kAllowedSystemCalls[] = {
    // memory
    SYS_brk, SYS_mmap, SYS_munmap, SYS_mremap, SYS_mprotect, SYS_madvise, SYS_mincore, SYS_msync, SYS_memfd_create,
    // threads and waiting
    SYS_futex, SYS_set_robust_list, SYS_get_robust_list, SYS_set_tid_address, SYS_rseq, SYS_sched_yield, SYS_sched_getaffinity, SYS_getcpu, SYS_nanosleep, SYS_clock_nanosleep,
    SYS_clock_gettime, SYS_clock_getres, SYS_gettimeofday, SYS_time, SYS_exit, SYS_exit_group, SYS_restart_syscall, SYS_arch_prctl,
    // descriptors
    SYS_read, SYS_write, SYS_readv, SYS_writev, SYS_pread64, SYS_pwrite64, SYS_preadv, SYS_pwritev, SYS_lseek, SYS_close, SYS_dup, SYS_dup2, SYS_dup3, SYS_fcntl, SYS_pipe, SYS_pipe2,
    SYS_eventfd2, SYS_epoll_create1, SYS_epoll_ctl, SYS_epoll_wait, SYS_epoll_pwait, SYS_epoll_pwait2, SYS_poll, SYS_ppoll, SYS_select, SYS_pselect6, SYS_timerfd_create,
    SYS_timerfd_settime, SYS_timerfd_gettime, SYS_ftruncate, SYS_fallocate, SYS_fsync, SYS_fdatasync, SYS_flock, SYS_fadvise64,
    // the connections it was given
    SYS_recvmsg, SYS_sendmsg, SYS_recvfrom, SYS_sendto, SYS_shutdown, SYS_getsockopt, SYS_setsockopt, SYS_getsockname, SYS_getpeername,
    // files (which of them is the file system policy's)
    SYS_open, SYS_openat, SYS_openat2, SYS_stat, SYS_fstat, SYS_lstat, SYS_newfstatat, SYS_statx, SYS_access, SYS_faccessat, SYS_faccessat2, SYS_readlink, SYS_readlinkat, SYS_getdents64,
    SYS_getcwd, SYS_statfs, SYS_fstatfs, SYS_mkdir, SYS_unlink, SYS_unlinkat, SYS_rename, SYS_renameat, SYS_renameat2, SYS_rmdir, SYS_umask,
    // signals, identity, limits
    SYS_rt_sigaction, SYS_rt_sigprocmask, SYS_rt_sigreturn, SYS_sigaltstack, SYS_rt_sigtimedwait, SYS_tgkill, SYS_getpid, SYS_getppid, SYS_gettid, SYS_getuid, SYS_geteuid, SYS_getgid,
    SYS_getegid, SYS_getrandom, SYS_uname, SYS_sysinfo, SYS_prlimit64, SYS_getrlimit, SYS_getrusage, SYS_times,
};

enum { kAllow = 0, kKill = 1, kClone, kClone3, kPrctl, kIoctl, kSocket, kCount };

class Program {
 public:
  void Add(uint16_t code, uint8_t jt, uint8_t jf, uint32_t k) { instructions_.push_back({code, jt, jf, k}); }
  size_t size() const { return instructions_.size(); }
  sock_filter& at(size_t i) { return instructions_[i]; }
  std::vector<sock_filter>& all() { return instructions_; }

 private:
  std::vector<sock_filter> instructions_;
};

uint32_t ArgumentLow(int index) { return static_cast<uint32_t>(offsetof(seccomp_data, args) + index * 8); }

std::vector<sock_filter> BuildFilter(uint32_t denied, const SandboxPolicy& policy) {
  Program p;
  const uint32_t kNr = offsetof(seccomp_data, nr), kArch = offsetof(seccomp_data, arch);
  // The architecture is the one the numbers are of, and not the x32 range of them.
  p.Add(BPF_LD | BPF_W | BPF_ABS, 0, 0, kArch);
  p.Add(BPF_JMP | BPF_JEQ | BPF_K, 1, 0, AUDIT_ARCH_X86_64);
  p.Add(BPF_RET | BPF_K, 0, 0, SECCOMP_RET_KILL_PROCESS);
  p.Add(BPF_LD | BPF_W | BPF_ABS, 0, 0, kNr);
  p.Add(BPF_JMP | BPF_JGE | BPF_K, 0, 1, 0x40000000);
  p.Add(BPF_RET | BPF_K, 0, 0, SECCOMP_RET_KILL_PROCESS);
  // Each allowed call: a jump over the next instruction where it is not that call, else a long jump to the end (patched below).
  std::vector<size_t> toAllow, toBlock[kCount];
  std::vector<int> allowed(std::begin(kAllowedSystemCalls), std::end(kAllowedSystemCalls));
  allowed.insert(allowed.end(), policy.extraSystemCalls.begin(), policy.extraSystemCalls.end());
  if (policy.anyIoctl) allowed.push_back(SYS_ioctl);
  if (!policy.executable.empty()) {
    for (int number : {SYS_execve, SYS_wait4, SYS_waitid, SYS_pipe2, SYS_dup2, SYS_dup3, SYS_close_range}) allowed.push_back(number);
  }
  if (policy.unixSockets) {
    // (A connection to the system's own: sockets of the network are not made, only these.)
    for (int number : {SYS_connect, SYS_getsockname, SYS_getpeername}) allowed.push_back(number);
  }
  for (int number : allowed) {
    p.Add(BPF_JMP | BPF_JEQ | BPF_K, 0, 1, static_cast<uint32_t>(number));
    toAllow.push_back(p.size());
    p.Add(BPF_JMP | BPF_JA, 0, 0, 0);
  }
  const struct {
    int number, block;
  } special[] = {{SYS_clone, kClone}, {SYS_clone3, kClone3}, {SYS_prctl, kPrctl}, {SYS_ioctl, kIoctl}, {SYS_socket, kSocket}};
  for (const auto& s : special) {
    if (s.number == SYS_ioctl && policy.anyIoctl) continue;
    if (s.number == SYS_socket && !policy.unixSockets) continue;
    p.Add(BPF_JMP | BPF_JEQ | BPF_K, 0, 1, static_cast<uint32_t>(s.number));
    toBlock[s.block].push_back(p.size());
    p.Add(BPF_JMP | BPF_JA, 0, 0, 0);
  }
  p.Add(BPF_RET | BPF_K, 0, 0, denied);
  const auto here = [&] { return p.size(); };
  const auto patch = [&](const std::vector<size_t>& from, size_t target) {
    for (size_t at : from) p.at(at).k = static_cast<uint32_t>(target - at - 1);
  };
  // clone: only what makes a thread (the flags glibc's pthread_create gives, and CLONE_THREAD among them), never a process or a namespace.
  {
    patch(toBlock[kClone], here());
    if (!policy.executable.empty()) {
      // (The clone of a vfork: the child shares the memory until it executes one of the programs it may.)
      p.Add(BPF_LD | BPF_W | BPF_ABS, 0, 0, ArgumentLow(0));
      p.Add(BPF_JMP | BPF_JEQ | BPF_K, 0, 1, CLONE_VM | CLONE_VFORK | SIGCHLD);
      toAllow.push_back(p.size());
      p.Add(BPF_JMP | BPF_JA, 0, 0, 0);
    }
    const uint32_t threadFlags = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD | CLONE_SYSVSEM | CLONE_SETTLS | CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID;
    p.Add(BPF_LD | BPF_W | BPF_ABS, 0, 0, ArgumentLow(0));
    p.Add(BPF_ALU | BPF_AND | BPF_K, 0, 0, ~threadFlags);
    p.Add(BPF_JMP | BPF_JEQ | BPF_K, 1, 0, 0);
    p.Add(BPF_RET | BPF_K, 0, 0, denied);
    p.Add(BPF_LD | BPF_W | BPF_ABS, 0, 0, ArgumentLow(0));
    p.Add(BPF_ALU | BPF_AND | BPF_K, 0, 0, CLONE_THREAD);
    p.Add(BPF_JMP | BPF_JEQ | BPF_K, 1, 0, CLONE_THREAD);
    p.Add(BPF_RET | BPF_K, 0, 0, denied);
    toAllow.push_back(p.size());
    p.Add(BPF_JMP | BPF_JA, 0, 0, 0);
  }
  // clone3 has its flags behind a pointer, which cannot be looked at: it is said not to be there, and the library falls back to clone.
  {
    patch(toBlock[kClone3], here());
    p.Add(BPF_RET | BPF_K, 0, 0, SECCOMP_RET_ERRNO | ENOSYS);
  }
  // prctl: naming a thread, the signal for a parent that is gone, and the few that are harmless.
  {
    patch(toBlock[kPrctl], here());
    p.Add(BPF_LD | BPF_W | BPF_ABS, 0, 0, ArgumentLow(0));
    for (uint32_t option : {1u /*PDEATHSIG*/, 2u /*GET_PDEATHSIG*/, 4u /*SET_DUMPABLE*/, 15u /*SET_NAME*/, 16u /*GET_NAME*/, 38u /*SET_NO_NEW_PRIVS*/, 39u /*GET_NO_NEW_PRIVS*/, 0x53564d41u /*SET_VMA*/}) {
      p.Add(BPF_JMP | BPF_JEQ | BPF_K, 0, 1, option);
      toAllow.push_back(p.size());
      p.Add(BPF_JMP | BPF_JA, 0, 0, 0);
    }
    p.Add(BPF_RET | BPF_K, 0, 0, denied);
  }
  // ioctl: asking how much is to be read, making a descriptor not block or close on exec, and asking if a stream is a terminal.
  {
    patch(toBlock[kIoctl], here());
    p.Add(BPF_LD | BPF_W | BPF_ABS, 0, 0, ArgumentLow(1));
    for (uint32_t request : {0x541Bu /*FIONREAD*/, 0x5421u /*FIONBIO*/, 0x5451u /*FIOCLEX*/, 0x5450u /*FIONCLEX*/, 0x5401u /*TCGETS*/}) {
      p.Add(BPF_JMP | BPF_JEQ | BPF_K, 0, 1, request);
      toAllow.push_back(p.size());
      p.Add(BPF_JMP | BPF_JA, 0, 0, 0);
    }
    p.Add(BPF_RET | BPF_K, 0, 0, SECCOMP_RET_ERRNO | ENOTTY);
  }
  // socket: only a unix one.
  if (policy.unixSockets) {
    patch(toBlock[kSocket], here());
    p.Add(BPF_LD | BPF_W | BPF_ABS, 0, 0, ArgumentLow(0));
    p.Add(BPF_JMP | BPF_JEQ | BPF_K, 1, 0, 1 /*AF_UNIX*/);
    p.Add(BPF_RET | BPF_K, 0, 0, denied);
    toAllow.push_back(p.size());
    p.Add(BPF_JMP | BPF_JA, 0, 0, 0);
  }
  patch(toAllow, here());
  p.Add(BPF_RET | BPF_K, 0, 0, SECCOMP_RET_ALLOW);
  return p.all();
}

// In report mode: the call that is not allowed is written out and fails with ENOSYS (the handler uses nothing but write and _exit).
void ReportDenied(int, siginfo_t* info, void* context) {
  char line[96];
  int n = 0;
  const char* prefix = "sandbox: system call not allowed: ";
  for (const char* c = prefix; *c; ++c) line[n++] = *c;
  int number = info->si_syscall;
  char digits[12];
  int d = 0;
  do { digits[d++] = static_cast<char>('0' + number % 10); number /= 10; } while (number > 0 && d < 11);
  while (d > 0) line[n++] = digits[--d];
  line[n++] = '\n';
  const ssize_t ignored = ::write(2, line, static_cast<size_t>(n));
  (void)ignored;
  static_cast<ucontext_t*>(context)->uc_mcontext.gregs[REG_RAX] = -ENOSYS;
}

bool EnterSeccomp(SandboxMode mode, const SandboxPolicy& policy, std::string& problems) {
  const uint32_t denied = mode == SandboxMode::Report ? SECCOMP_RET_TRAP : SECCOMP_RET_KILL_PROCESS;
  if (mode == SandboxMode::Report) {
    struct sigaction action{};
    action.sa_sigaction = ReportDenied;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    ::sigaction(SIGSYS, &action, nullptr);
  }
  std::vector<sock_filter> filter = BuildFilter(denied, policy);
  sock_fprog program{static_cast<unsigned short>(filter.size()), filter.data()};
  if (::syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_TSYNC, &program) != 0) {
    problems += std::string("seccomp: ") + std::strerror(errno) + "; ";
    return false;
  }
  return true;
}

}  // namespace

SandboxMode SandboxModeFromEnvironment() {
  const char* value = std::getenv("SOLAR_SANDBOX");
  if (!value) return SandboxMode::On;
  if (std::strcmp(value, "off") == 0 || std::strcmp(value, "0") == 0) return SandboxMode::Off;
  if (std::strcmp(value, "report") == 0) return SandboxMode::Report;
  return SandboxMode::On;
}

SandboxResult EnterSandbox(const SandboxPolicy& policy, SandboxMode mode) {
  SandboxResult result;
  if (mode == SandboxMode::Off) return result;
  result.namespaces = EnterNamespaces(result.problems);
  result.noNewPrivileges = ::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0;
  if (!result.noNewPrivileges) {
    result.problems += "no new privileges: not set; ";
    return result;
  }
  result.landlock = EnterLandlock(policy, result.problems);
  result.seccomp = EnterSeccomp(mode, policy, result.problems);
  return result;
}

}  // namespace solar::gui

#else

namespace solar::gui {

SandboxMode SandboxModeFromEnvironment() {
  const char* value = std::getenv("SOLAR_SANDBOX");
  return value && (std::strcmp(value, "off") == 0 || std::strcmp(value, "0") == 0) ? SandboxMode::Off : SandboxMode::On;
}

SandboxResult EnterSandbox(const SandboxPolicy&, SandboxMode mode) {
  SandboxResult result;
  if (mode != SandboxMode::Off) result.problems = "the sandbox is for Linux on x86-64; ";
  return result;
}

}  // namespace solar::gui

#endif
