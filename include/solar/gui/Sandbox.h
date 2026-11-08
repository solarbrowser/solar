/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <string>
#include <vector>

// What the page's process is shut in once it has started (Linux): it has what it was given (its connections, its memory) and the files it may read, and nothing of the rest of the machine,
// so that a page that takes over the process can do no more than the process could. Four layers, each of which is in the way of what the one before it lets through:
//   - new namespaces (user, network, IPC, host name) with no capabilities in them: no network of any kind, not the abstract sockets of the window system, no one else's IPC;
//   - Landlock: of the file system only what the policy lists (the rest is not there to be opened, made or removed), no TCP, no signals or abstract sockets out of the process;
//   - seccomp: only the system calls the page's process makes (everything else ends it), and the clone that makes a thread but none that makes a process or a namespace;
//   - no new privileges, and the capabilities gone.
namespace solar::gui {

struct SandboxPolicy {
  // Files and directories (with what is under them) that may be read.
  std::vector<std::string> readable;
  // Directories (with what is under them) that may be read and written, made in and removed from.
  std::vector<std::string> writable;
  // What a process needs more than the page's has (the GPU's process, which talks to a driver): system calls (numbers of the platform) that are allowed besides, every ioctl (the drivers'
  // are the ones of their devices, and are many), and unix sockets (a connection to what the driver may ask of the system: nothing of the network).
  std::vector<int> extraSystemCalls;
  bool anyIoctl = false;
  bool unixSockets = false;
  // Programs the process may start (and only these: a process is made the way a library does, with the child sharing the memory until it executes), for what a driver runs when its
  // devices are not as it likes (they stay as shut in as the process is: nothing is gained by it).
  std::vector<std::string> executable;
};

enum class SandboxMode {
  Off,     // nothing is done (for looking into the process with tools that need it)
  Report,  // as On, but a system call that is not allowed is written to stderr and fails with ENOSYS, and the process goes on (for finding what a policy lacks)
  On,      // a system call that is not allowed ends the process
};

// What the sandbox came to.
struct SandboxResult {
  bool seccomp = false, landlock = false, namespaces = false, noNewPrivileges = false;
  // What could not be done, for the log (empty where all was).
  std::string problems;
};

// Enters the sandbox for this process. To be called before any thread other than the calling one is made (what restricts a thread is inherited by the threads it makes, not given to the
// ones that exist). The seccomp layer is not optional: where it cannot be made `seccomp` is false and the caller is to end the process; the others are made where the kernel has them.
SandboxResult EnterSandbox(const SandboxPolicy& policy, SandboxMode mode);

// SOLAR_SANDBOX=off|report|on (on if it is not set).
SandboxMode SandboxModeFromEnvironment();

}  // namespace solar::gui
