/*
 * A small tracer for finding what a program asks of the system (what a sandbox has to allow): it runs the command, follows every process and thread it makes, and writes, for each process
 * that was started with the argument given as --only (or for all), the numbers of the system calls it made and the paths it named. Linux, x86-64.
 *
 *   tools/syscall-census OUT.txt --only --renderer -- ./solar file:///page.html
 *
 * OUT.txt also gets "E <pid> <call> <path> <errno>" for each of those that failed. OUT.txt gets lines "S <number> <count>" and "P <call> <path>" for each such process, after a line "== <pid> <command line>".
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAXPID 65536
#define SLOT(pid) ((pid) & 0xFFFF)  /* (process numbers go well past 65536: the slots are shared by few that are alive together) */

struct proc {
  int used, tgid, matches, inSyscall;
  char lastPath[512], lastCall[16];
  long lastNr; unsigned long lastArg0;
  unsigned long counts[512];
};
static struct proc* procs[MAXPID];
static const char* only = NULL;
static FILE* out;
static char cmdlines[MAXPID][256];
static int cmdlineKnown[MAXPID];

static void readCmdline(int pid) {
  char path[64];
  const int slot = SLOT(pid);
  snprintf(path, sizeof path, "/proc/%d/cmdline", pid);
  FILE* f = fopen(path, "r");
  if (!f) return;
  size_t n = fread(cmdlines[slot], 1, 255, f);
  fclose(f);
  for (size_t i = 0; i < n; ++i) if (cmdlines[slot][i] == 0) cmdlines[slot][i] = ' ';
  cmdlines[slot][n] = 0;
  cmdlineKnown[slot] = 1;
}

static int readString(int pid, unsigned long address, char* into, size_t size) {
  struct iovec local = {into, size - 1}, remote = {(void*)address, size - 1};
  ssize_t n = process_vm_readv(pid, &local, 1, &remote, 1, 0);
  if (n <= 0) return 0;
  into[n] = 0;
  into[strnlen(into, n)] = 0;
  return 1;
}


int main(int argc, char** argv) {
  if (argc < 4) { fprintf(stderr, "usage: syscall-census OUT [--only STRING] -- command...\n"); return 2; }
  out = fopen(argv[1], "w");
  int i = 2;
  if (strcmp(argv[i], "--only") == 0) { only = argv[i + 1]; i += 2; }
  if (strcmp(argv[i], "--") == 0) ++i;
  pid_t child = fork();
  if (child == 0) {
    ptrace(PTRACE_TRACEME, 0, 0, 0);
    raise(SIGSTOP);
    execvp(argv[i], argv + i);
    _exit(127);
  }
  int status;
  waitpid(child, &status, 0);
  ptrace(PTRACE_SETOPTIONS, child, 0, PTRACE_O_TRACESYSGOOD | PTRACE_O_TRACEFORK | PTRACE_O_TRACEVFORK | PTRACE_O_TRACECLONE | PTRACE_O_TRACEEXEC | PTRACE_O_EXITKILL);
  ptrace(PTRACE_SYSCALL, child, 0, 0);
  for (;;) {
    pid_t pid = waitpid(-1, &status, __WALL);
    if (pid < 0) break;
    if (!procs[SLOT(pid)]) { procs[SLOT(pid)] = calloc(1, sizeof(struct proc)); }
    struct proc* p = procs[SLOT(pid)];
    if (WIFSIGNALED(status)) fprintf(out, "X %d killed by signal %d (its last system call was %ld, first argument 0x%lx)\n", pid, WTERMSIG(status), p->lastNr, p->lastArg0);
    if (WIFEXITED(status) && WEXITSTATUS(status) != 0) fprintf(out, "X %d exit %d\n", pid, WEXITSTATUS(status));
    if (WIFEXITED(status) || WIFSIGNALED(status)) continue;
    int sig = WSTOPSIG(status);
    int deliver = 0;
    if (sig == (SIGTRAP | 0x80)) {
      struct user_regs_struct r;
      if (ptrace(PTRACE_GETREGS, pid, 0, &r) == 0) {
        p->inSyscall = !p->inSyscall;
        if (!p->inSyscall) {
          long result = (long)r.rax;
          if (p->lastPath[0] && result < 0 && result > -4096) fprintf(out, "E %d %s %s %ld\n", pid, p->lastCall, p->lastPath, -result);
          p->lastPath[0] = 0;
        }
        if (p->inSyscall) {
          if (!cmdlineKnown[SLOT(pid)]) readCmdline(pid);
          unsigned long number = r.orig_rax;
          p->lastNr = (long)number;
          p->lastArg0 = r.rdi;
          if (number < 512) p->counts[number]++;
          char path[512];
          unsigned long where = 0;
          const char* name = NULL;
          switch (number) {
            case 2: where = r.rdi; name = "open"; break;
            case 257: where = r.rsi; name = "openat"; break;
            case 262: where = r.rsi; name = "newfstatat"; break;
            case 332: where = r.rsi; name = "statx"; break;
            case 4: case 6: where = r.rdi; name = "stat"; break;
            case 21: where = r.rdi; name = "access"; break;
            case 269: where = r.rsi; name = "faccessat"; break;
            case 89: where = r.rdi; name = "readlink"; break;
            case 267: where = r.rsi; name = "readlinkat"; break;
            case 80: where = r.rdi; name = "chdir"; break;
            case 59: where = r.rdi; name = "execve"; break;
            case 83: where = r.rdi; name = "mkdir"; break;
            case 87: where = r.rdi; name = "unlink"; break;
            case 82: where = r.rdi; name = "rename"; break;
            case 42: where = r.rsi + 2; name = "connect"; break;  /* (the path of a unix socket; an abstract one starts with a nul) */
            case 88: where = r.rdi; name = "symlink"; break;
          }
          p->lastPath[0] = 0;
          if (name && readString(pid, where, path, sizeof path)) { fprintf(out, "P %d %s %s\n", pid, name, path); strncpy(p->lastPath, path, 511); strncpy(p->lastCall, name, 15); }
        }
      }
    } else if (sig == SIGTRAP) {
      int event = status >> 16;
      if (event == PTRACE_EVENT_EXEC) { cmdlineKnown[SLOT(pid)] = 0; readCmdline(pid); }
    } else if (sig != SIGSTOP) {
      deliver = sig;
    }
    ptrace(PTRACE_SYSCALL, pid, 0, deliver);
  }
  for (int pid = 0; pid < MAXPID; ++pid) {
    if (!procs[pid]) continue;
    if (only && !strstr(cmdlines[pid], only)) continue;
    fprintf(out, "== %d %s\n", pid, cmdlines[pid]);
    for (int n = 0; n < 512; ++n) if (procs[pid]->counts[n]) fprintf(out, "S %d %lu\n", n, procs[pid]->counts[n]);
  }
  fclose(out);
  return 0;
}
