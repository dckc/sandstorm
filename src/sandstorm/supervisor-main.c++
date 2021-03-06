// Sandstorm - Personal Cloud Sandbox
// Copyright (c) 2014 Sandstorm Development Group, Inc. and contributors
// All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Hack around stdlib bug with C++14.
#include <initializer_list>  // force libstdc++ to include its config
#undef _GLIBCXX_HAVE_GETS    // correct broken config
// End hack.

#include <kj/main.h>
#include <kj/debug.h>
#include <kj/async-io.h>
#include <kj/async-unix.h>
#include <kj/io.h>
#include <capnp/rpc-twoparty.h>
#include <capnp/rpc.capnp.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/capability.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <netinet/in.h>
#include <net/if.h>
#include <linux/sockios.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <limits.h>
#include <sched.h>
#include <dirent.h>
#include <pwd.h>
#include <grp.h>
#include <sys/inotify.h>
#include <seccomp.h>
#include <map>
#include <unordered_map>

#include <sandstorm/grain.capnp.h>
#include <sandstorm/supervisor.capnp.h>

#include "version.h"
#include "send-fd.h"

// In case kernel headers are old.
#ifndef PR_SET_NO_NEW_PRIVS
#define PR_SET_NO_NEW_PRIVS 38
#endif

namespace sandstorm {

typedef unsigned int uint;

static kj::AutoCloseFd raiiOpen(kj::StringPtr name, int flags, mode_t mode = 0666) {
  int fd;
  KJ_SYSCALL(fd = open(name.cStr(), flags, mode), name);
  return kj::AutoCloseFd(fd);
}

// =======================================================================================
// Directory size watcher

class DiskUsageWatcher {
  // Class which watches a directory tree, counts up the total disk usage, and fires events when
  // it changes. Uses inotify. Which turns out to be... harder than it should be.

public:
  DiskUsageWatcher(kj::UnixEventPort& eventPort): eventPort(eventPort) {}

  kj::Promise<void> init() {
    // Start watching the current directory.

    // Note: this function is also called to restart watching from scratch when the inotify event
    //   queue overflows (hopefully rare).

    int fd;
    KJ_SYSCALL(fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC));
    inotifyFd = kj::AutoCloseFd(fd);
    totalSize = 0;
    watchMap.clear();
    pendingWatches.add(nullptr);  // root directory
    return readLoop();
  }

  uint64_t getSize() { return totalSize; }

  kj::Promise<uint64_t> getSizeWhenChanged(uint64_t oldSize) {
    kj::Promise<void> trigger = nullptr;
    if (totalSize == oldSize) {
      auto paf = kj::newPromiseAndFulfiller<void>();
      trigger = kj::mv(paf.promise);
      listeners.add(kj::mv(paf.fulfiller));
    } else {
      trigger = kj::READY_NOW;
    }

    // Even when the value has changed, wait 100ms so that we're not streaming tons of updates
    // whenever there is heavy disk I/O. This is just for a silly display anyway.
    return trigger.then([this]() {
      return eventPort.atSteadyTime(eventPort.steadyTime() + 100 * kj::MILLISECONDS);
    }).then([this]() {
      return totalSize;
    });
  }

private:
  kj::UnixEventPort& eventPort;
  kj::AutoCloseFd inotifyFd;
  uint64_t totalSize;

  uint64_t lastUpdateSize = kj::maxValue;  // value of totalSize last time listeners were fired.
  kj::Vector<kj::Own<kj::PromiseFulfiller<void>>> listeners;

  struct ChildInfo {
    kj::String name;
    uint64_t size;
  };
  struct WatchInfo {
    kj::String path;  // null = root directory
    std::map<kj::StringPtr, ChildInfo> childSizes;
  };
  std::unordered_map<int, WatchInfo> watchMap;
  // Maps inotify watch descriptors to info about what is being watched.

  kj::Vector<kj::String> pendingWatches;
  // Directories we would like to watch, but we can't add watches on them just yet because we need
  // to finish processing a list of events received from inotify before we mess with the watch
  // descriptor table.

  void addPendingWatches() {
    // Start watching everything that has been added to the pendingWatches list.

    // We treat pendingWatches as a stack here in order to get DFS traversal of the directory tree.
    while (pendingWatches.size() > 0) {
      auto path = kj::mv(pendingWatches.end()[-1]);
      pendingWatches.removeLast();
      addWatch(kj::mv(path));
    }
  }

  void addWatch(kj::String&& path) {
    // Start watching `path`. This is idempotent -- it's safe to watch the same path multiple
    // times.

    static const uint32_t FLAGS =
        IN_CREATE | IN_DELETE | IN_MODIFY | IN_MOVED_FROM | IN_MOVED_TO |
        IN_DONT_FOLLOW | IN_ONLYDIR | IN_EXCL_UNLINK;

    for (;;) {
      const char* pathPtr = path == nullptr ? "." : path.cStr();
      int wd = inotify_add_watch(inotifyFd, pathPtr,
          FLAGS | IN_DONT_FOLLOW | IN_EXCL_UNLINK);

      if (wd >= 0) {
        WatchInfo& watchInfo = watchMap[wd];

        // Update the watch map. Note that it's possible that inotify_add_watch() returned a
        // pre-existing watch descriptor, if we tried to add a watch on a directory we're
        // already watching. This can happen in various race conditions. Replacing the path is
        // actually exactly what we want to do in these cases anyway.
        watchInfo.path = kj::mv(path);

        // In the case that we are reusing an existing watch descriptor, we want to clear out the
        // existing contents as they may be stale due to, again, race conditions.
        for (auto& child: watchInfo.childSizes) {
          totalSize -= child.second.size;
        }
        watchInfo.childSizes.clear();

        // Now repopulate the children by listing the directory.
        DIR* dir = opendir(pathPtr);
        if (dir != nullptr) {
          KJ_DEFER(closedir(dir));
          for (;;) {
            errno = 0;
            struct dirent* entry = readdir(dir);
            if (entry == nullptr) {
              int error = errno;
              if (error == 0) {
                break;
              } else {
                KJ_FAIL_SYSCALL("readdir", error, pathPtr);
              }
            }

            kj::StringPtr name = entry->d_name;
            if (name != "." && name != "..") {
              childEvent(watchInfo, name);
            }
          }
        }

        return;
      }

      // Error occurred.
      int error = errno;
      switch (error) {
        case EINTR:
          // Keep trying.
          break;

        case ENOENT:
        case ENOTDIR:
          // Apparently there is no longer a directory at this path. Perhaps it was deleted.
          // No matter.
          return;

        case ENOSPC:
          // No more inotify watches available.
          // TODO(someday): Revert to some sort of polling mode? For now, fall through to error
          //   case.
        default:
          KJ_FAIL_SYSCALL("inotify_add_watch", error, path);
      }
    }
  }

  kj::Promise<void> readLoop() {
    addPendingWatches();
    maybeFireEvents();
    return eventPort.onFdEvent(inotifyFd, POLLIN).then([this](short) {
      alignas(uint64_t) kj::byte buffer[4096];

      for (;;) {
        ssize_t n;
        KJ_NONBLOCKING_SYSCALL(n = read(inotifyFd, buffer, sizeof(buffer)));

        if (n < 0) {
          // EAGAIN; try again later.
          return readLoop();
        }

        KJ_ASSERT(n > 0, "inotify EOF?");

        kj::byte* pos = buffer;
        while (n > 0) {
          // Split off one event.
          auto event = reinterpret_cast<struct inotify_event*>(pos);
          size_t eventSize = sizeof(struct inotify_event) + event->len;
          KJ_ASSERT(eventSize <= n, "inotify returned partial event?");
          KJ_ASSERT(eventSize % sizeof(size_t) == 0, "inotify event not aligned?");
          n -= eventSize;
          pos += eventSize;

          if (event->mask & IN_Q_OVERFLOW) {
            // Queue overflow; start over from scratch.
            inotifyFd = nullptr;
            KJ_LOG(WARNING, "inotify event queue overflow; restarting watch from scratch");
            return init();
          }

          auto iter = watchMap.find(event->wd);
          KJ_ASSERT(iter != watchMap.end(), "inotify gave unknown watch descriptor?");

          if (event->mask & (IN_CREATE | IN_DELETE | IN_MODIFY | IN_MOVE)) {
            childEvent(iter->second, event->name);
          }

          if (event->mask & IN_IGNORED) {
            // This watch descriptor is being removed, probably because it was deleted.

            // There shouldn't be any children left, but if there are, go ahead and un-count them.
            for (auto& child: iter->second.childSizes) {
              totalSize -= child.second.size;
            }

            watchMap.erase(iter);
          }
        }
      }
    });
  }

  void childEvent(WatchInfo& watchInfo, kj::StringPtr name) {
    // Called to update the child table when we receive an inotify event with the given name.

    // OK, we received notification that something happened to the child named `name`.
    // Unfortunately, we don't have any idea how long ago this event happened. Worse, any
    // number of other events may have occurred since this one was generated. For example,
    // the event may have been on a file that has subsequently been deleted, and maybe even
    // recreated as a different kind of node. If we lstat() it, we get information about
    // what is currently on disk, not whatever generated this event.
    //
    // Therefore, the inotify event mask is mostly useless. We can only use the event as a hint
    // that something happened at this child. We have to compare what we know about the child
    // vs. what we knew in the past to determine what has changed. Note that if inotify
    // provided a `struct stat` along with the event then we wouldn't have this problem!

    auto usage = getDiskUsage(watchInfo.path, name);
    totalSize += usage.bytes;

    auto iter = watchInfo.childSizes.find(name);
    if (usage.bytes == 0) {
      // There is no longer a child by this name on disk. Remove whatever is in the map.
      if (iter != watchInfo.childSizes.end()) {
        totalSize -= iter->second.size;
        watchInfo.childSizes.erase(iter);
      }
    } else if (iter == watchInfo.childSizes.end()) {
      // There is a child by this name on disk, but not in the map. Add it.
      ChildInfo newChild = { kj::heapString(name), usage.bytes };
      kj::StringPtr namePtr = newChild.name;
      KJ_ASSERT(watchInfo.childSizes.insert(std::make_pair(namePtr, kj::mv(newChild))).second);
    } else {
      // There is a child by this name on disk and in the map. Check for a change in size.
      totalSize -= iter->second.size;
      iter->second.size = usage.bytes;
    }

    // If the child is a directory, plan to start watching it later. Note that IN_MODIFY events
    // are not generated for subdirectories (only files), so if we got an event on a directory it
    // must be create, move to, move from, or delete. In the latter two cases, the node wouldn't
    // exist anymore, so usage.isDir would be false. So, we know this directory is either
    // newly-created or newly moved in from elsewhere. In the creation case, we clearly need to
    // start watching the directory. In the moved-in case, we are probably already watching the
    // directory, however it is necessary to redo the watch because the path has changed and the
    // directory state may have become inconsistent in the time that the path was wrong.
    if (usage.isDir) {
      // We can't actually add the new watch now because we need to process the remaining
      // events from the last read() in order to make sure we're caught up with inotify's
      // state.
      pendingWatches.add(kj::mv(usage.path));
    }
  }

  struct DiskUsage {
    kj::String path;
    uint64_t bytes;
    bool isDir;
  };

  DiskUsage getDiskUsage(kj::StringPtr parent, kj::StringPtr name) {
    // Get the disk usage of the given file within the given parent directory. This is not exactly
    // the file size; it also includes estimates of storage overhead, such as rounding up to the
    // block size. If the file no longer exists, its size is reported as zero.

    kj::String path = parent == nullptr ? kj::heapString(name) : kj::str(parent, '/', name);
    for (;;) {
      struct stat stats;
      if (lstat(path.cStr(), &stats) >= 0) {
        // Success.

        DiskUsage result;
        result.path = kj::mv(path);
        result.isDir = S_ISDIR(stats.st_mode);

        // Round the size up to the nearest block; we assume 4k blocks.
        result.bytes = (stats.st_size + 4095) & ~4095ull;

        // Divide by link count so that files with many hardlinks aren't overcounted.
        result.bytes /= stats.st_nlink;

        // Add sizeof(stats) to approximate the directory entry overhead, and also add
        // the size of the null-terminated filename rounded up to a word.
        result.bytes += sizeof(stats) + ((name.size() + 8) & ~7ull);

        return result;
      }

      // There was an error.
      int error = errno;
      switch (error) {
        case EINTR:
          // continue loop
          break;
        case ENOENT:   // File no longer exists...
        case ENOTDIR:  // ... and a parent directory was replaced.
          return {kj::mv(path), 0, false};
        default:
          // Default
          KJ_FAIL_SYSCALL("lstat", error, path);
      }
    }
  }

  void maybeFireEvents() {
    if (totalSize != lastUpdateSize) {
      for (auto& listener: listeners) {
        listener->fulfill();
      }
      listeners.resize(0);
      lastUpdateSize = totalSize;
    }
  }
};

// =======================================================================================
// Termination handling:  Must kill child if parent terminates.
//
// We also terminate automatically if we don't receive any keep-alives in a 5-minute interval.

pid_t childPid = 0;
bool keepAlive = true;

void logSafely(const char* text) {
  // Log a message in an async-signal-safe way.

  while (text[0] != '\0') {
    ssize_t n = write(STDERR_FILENO, text, strlen(text));
    if (n < 0) return;
    text += n;
  }
}

#define SANDSTORM_LOG(text) \
  logSafely("** SANDSTORM SUPERVISOR: " text "\n")

void killChild() {
  if (childPid != 0) {
    kill(childPid, SIGKILL);
    childPid = 0;
  }

  // We don't have to waitpid() because when we exit the child will be adopted by init which will
  // automatically reap it.
}

void killChildAndExit(int status) KJ_NORETURN;
void killChildAndExit(int status) {
  killChild();

  // TODO(cleanup):  Decide what exit status is supposed to mean.  Maybe it should just always be
  //   zero?
  _exit(status);
}

void signalHandler(int signo) {
  switch (signo) {
    case SIGALRM:
      if (keepAlive) {
        SANDSTORM_LOG("Grain still in use; staying up for now.");
        keepAlive = false;
        return;
      }
      SANDSTORM_LOG("Grain no longer in use; shutting down.");
      killChildAndExit(0);

    case SIGINT:
    case SIGTERM:
      SANDSTORM_LOG("Grain supervisor terminated by signal.");
      killChildAndExit(0);

    default:
      // Some signal that should cause death.
      SANDSTORM_LOG("Grain supervisor crashed due to signal.");
      killChildAndExit(1);
  }
}

int DEATH_SIGNALS[] = {
  // All signals that by default terminate the process.
  SIGHUP, SIGINT, SIGQUIT, SIGILL, SIGABRT, SIGFPE, SIGSEGV, SIGTERM, SIGUSR1, SIGUSR2, SIGBUS,
  SIGPOLL, SIGPROF, SIGSYS, SIGTRAP, SIGVTALRM, SIGXCPU, SIGXFSZ, SIGSTKFLT, SIGPWR
};

void registerSignalHandlers() {
  // Create a sigaction that runs our signal handler with all signals blocked.  Our signal handler
  // completes (or exits) quickly anyway, so let's not try to deal with it being interruptable.
  struct sigaction action;
  memset(&action, 0, sizeof(action));
  action.sa_handler = &signalHandler;
  sigfillset(&action.sa_mask);

  // SIGALRM will fire every five minutes and will kill us if no keepalive was received in that
  // time.
  KJ_SYSCALL(sigaction(SIGALRM, &action, nullptr));

  // Other death signals simply kill us immediately.
  for (int signo: kj::ArrayPtr<int>(DEATH_SIGNALS)) {
    KJ_SYSCALL(sigaction(signo, &action, nullptr));
  }

  // Set up the SIGALRM timer.  Note that this is not inherited over fork.
  struct itimerval timer;
  memset(&timer, 0, sizeof(timer));
  timer.it_interval.tv_sec = 300;
  timer.it_value.tv_sec = 300;
  KJ_SYSCALL(setitimer(ITIMER_REAL, &timer, nullptr));
}

// =======================================================================================

class SupervisorMain {
  // Main class for the Sandstorm supervisor.  This program:
  // - Sets up a sandbox for a grain.
  // - Executes the grain in the sandbox.
  // - Implements the platform API for the grain.
  // - Exposes a network interface through which the rest of the platform can talk to the grain.
  //
  // The supervisor places itself into the same sandbox as the grain, except that the supervisor
  // gets network access whereas the grain does not (the grain can only communicate with the world
  // through the supervisor).

public:
  SupervisorMain(kj::ProcessContext& context): context(context) {
    // Make sure we didn't inherit a weird signal mask from the parent process.  Gotta do this as
    // early as possible so as not to confuse KJ code that deals with signals.
    sigset_t sigset;
    KJ_SYSCALL(sigemptyset(&sigset));
    KJ_SYSCALL(sigprocmask(SIG_SETMASK, &sigset, nullptr));
  }

  kj::MainFunc getMain() {
    return kj::MainBuilder(context, "Sandstorm version " SANDSTORM_VERSION,
                           "Runs a Sandstorm grain supervisor for the grain <grain-id>, which is "
                           "an instance of app <app-id>.  Executes <command> inside the grain "
                           "sandbox.")
        .addOptionWithArg({"pkg"}, KJ_BIND_METHOD(*this, setPkg), "<path>",
                          "Set directory containing the app package.  "
                          "Defaults to '$SANDSTORM_HOME/var/sandstorm/apps/<app-name>'.")
        .addOptionWithArg({"var"}, KJ_BIND_METHOD(*this, setVar), "<path>",
                          "Set directory where grain's mutable persistent data will be stored.  "
                          "Defaults to '$SANDSTORM_HOME/var/sandstorm/grains/<grain-id>'.")
        .addOptionWithArg({'e', "env"}, KJ_BIND_METHOD(*this, addEnv), "<name>=<val>",
                          "Set the environment variable <name> to <val> inside the sandbox.  Note "
                          "that *no* environment variables are set by default.")
        .addOption({"proc"}, [this]() { setMountProc(true); return true; },
                   "Mount procfs inside the sandbox.  For security reasons, this is NOT "
                   "RECOMMENDED during normal use, but it may be useful for debugging.")
        .addOption({"stdio"}, [this]() { keepStdio = true; return true; },
                   "Don't redirect the sandbox's stdio.  Useful for debugging.")
        .addOption({"dev"}, [this]() { devmode = true; return true; },
                   "Allow some system calls useful for debugging which are blocked in production.")
        .addOption({"seccomp-dump-pfc"}, [this]() { seccomp_dump_pfc = true; return true; },
                   "Dump libseccomp PFC output.")
        .addOption({'n', "new"}, [this]() { setIsNew(true); return true; },
                   "Initializes a new grain.  (Otherwise, runs an existing one.)")
        .expectArg("<app-name>", KJ_BIND_METHOD(*this, setAppName))
        .expectArg("<grain-id>", KJ_BIND_METHOD(*this, setGrainId))
        .expectOneOrMoreArgs("<command>", KJ_BIND_METHOD(*this, addCommandArg))
        .callAfterParsing(KJ_BIND_METHOD(*this, run))
        .build();
  }

  // =====================================================================================
  // Flag handlers

  void setIsNew(bool isNew) {
    this->isNew = isNew;
  }

  void setMountProc(bool mountProc) {
    if (mountProc) {
      context.warning("WARNING: --proc is dangerous.  Only use it when debugging code you trust.");
    }
    this->mountProc = mountProc;
  }

  kj::MainBuilder::Validity setAppName(kj::StringPtr name) {
    if (name == nullptr || name.findFirst('/') != nullptr) {
      return "Invalid app name.";
    }
    appName = kj::heapString(name);
    return true;
  }

  kj::MainBuilder::Validity setGrainId(kj::StringPtr id) {
    if (id == nullptr || id.findFirst('/') != nullptr) {
      return "Invalid grain id.";
    }
    grainId = kj::heapString(id);
    return true;
  }

  kj::MainBuilder::Validity setPkg(kj::StringPtr path) {
    pkgPath = realPath(kj::heapString(path));
    return true;
  }

  kj::MainBuilder::Validity setVar(kj::StringPtr path) {
    varPath = realPath(kj::heapString(path));
    return true;
  }

  kj::MainBuilder::Validity addEnv(kj::StringPtr arg) {
    environment.add(kj::heapString(arg));
    return true;
  }

  kj::MainBuilder::Validity addCommandArg(kj::StringPtr arg) {
    command.add(kj::heapString(arg));
    return true;
  }

  // =====================================================================================

  kj::MainBuilder::Validity run() {
    setupSupervisor();

    checkIfAlreadyRunning();  // Exits if another supervisor is still running in this sandbox.

    SANDSTORM_LOG("Starting up grain.");

    registerSignalHandlers();

    // Allocate the API socket.
    int fds[2];
    KJ_SYSCALL(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds));

    // Now time to run the start command, in a further chroot.
    KJ_SYSCALL(childPid = fork());
    if (childPid == 0) {
      // We're in the child.
      KJ_SYSCALL(close(fds[0]));  // just to be safe, even though it's CLOEXEC.
      runChild(fds[1]);
    } else {
      // We're in the supervisor.
      KJ_DEFER(killChild());
      KJ_SYSCALL(close(fds[1]));
      runSupervisor(fds[0]);
    }
  }

private:
  kj::ProcessContext& context;

  kj::String appName;
  kj::String grainId;
  kj::String pkgPath;
  kj::String varPath;
  kj::Vector<kj::String> command;
  kj::Vector<kj::String> environment;
  bool isNew = false;
  bool mountProc = false;
  bool keepStdio = false;
  bool devmode = false;
  bool seccomp_dump_pfc = false;

  void bind(kj::StringPtr src, kj::StringPtr dst, unsigned long flags = 0) {
    // Contrary to the documentation of MS_BIND claiming this is no longer the case after 2.6.26,
    // mountflags are ignored on the initial bind.  We have to issue a subsequent remount to set
    // them.
    KJ_SYSCALL(mount(src.cStr(), dst.cStr(), nullptr, MS_BIND, nullptr), src, dst);
    KJ_SYSCALL(mount(src.cStr(), dst.cStr(), nullptr,
                     MS_BIND | MS_REMOUNT | MS_NOSUID | MS_NOATIME | flags, nullptr),
        src, dst);
  }

  kj::String realPath(kj::StringPtr path) {
    char* cResult = realpath(path.cStr(), nullptr);
    if (cResult == nullptr) {
      int error = errno;
      if (error != ENOENT) {
        KJ_FAIL_SYSCALL("realpath", error, path);
      }

      // realpath() fails if the target doesn't exist, but our goal here is just to convert a
      // relative path to absolute whether it exists or not. So try resolving the parent instead.
      KJ_IF_MAYBE(slashPos, path.findLast('/')) {
        if (*slashPos == 0) {
          // Path is e.g. "/foo". The root directory obviously exists.
          return kj::heapString(path);
        } else {
          return kj::str(realPath(kj::heapString(path.slice(0, *slashPos))),
                         path.slice(*slashPos));
        }
      } else {
        // Path is a relative path with only one component.
        char* cwd = getcwd(nullptr, 0);
        KJ_DEFER(free(cwd));
        if (cwd[0] == '/' && cwd[1] == '\0') {
          return kj::str('/', path);
        } else {
          return kj::str(cwd, '/', path);
        }
      }
    }
    auto result = kj::heapString(cResult);
    free(cResult);
    return result;
  }

  // =====================================================================================

  void setupSupervisor() {
    // Enable no_new_privs so that once we drop privileges we can never regain them through e.g.
    // execing a suid-root binary.  Sandboxed apps should not need that.
    KJ_SYSCALL(prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0));

    closeFds();
    checkPaths();
    unshareOuter();
    setupFilesystem();
    setupStdio();

    // Note:  permanentlyDropSuperuser() is performed post-fork; see comment in function def.
  }

  void closeFds() {
    // Close all unexpected file descriptors (i.e. other than stdin/stdout/stderr).  This is a
    // safety measure incase we were launched by a badly-written parent program which forgot to
    // set CLOEXEC on its private file descriptors.  We don't want the sandboxed process to
    // accidentally get access to those.

    // We detect open file descriptors by reading from /proc.
    //
    // We need to defer closing each FD until after the scan completes, because:
    // 1) We probably shouldn't change the directory contents while listing.
    // 2) opendir() itself opens an FD.  Closing it would disrupt the scan.
    kj::Vector<int> fds;

    {
      DIR* dir = opendir("/proc/self/fd");
      if (dir == nullptr) {
        KJ_FAIL_SYSCALL("opendir(/proc/self/fd)", errno);
      }
      KJ_DEFER(KJ_SYSCALL(closedir(dir)) { break; });

      for (;;) {
        struct dirent entry;
        struct dirent* eptr = nullptr;
        int error = readdir_r(dir, &entry, &eptr);
        if (error != 0) {
          KJ_FAIL_SYSCALL("readdir_r(/proc/self/fd)", error);
        }
        if (eptr == nullptr) {
          // End of directory.
          break;
        }

        if (eptr->d_name[0] != '.') {
          char* end;
          int fd = strtoul(eptr->d_name, &end, 10);
          KJ_ASSERT(*end == '\0' && end > eptr->d_name,
                    "File in /proc/self/fd had non-numeric name?", eptr->d_name);
          if (fd > STDERR_FILENO) {
            fds.add(fd);
          }
        }
      }
    }

    for (int fd: fds) {
      // Ignore close errors -- we don't care, as long as the file is closed.  (Also, one close()
      // will always return EBADF because it's the directory FD closed in closedir().)
      close(fd);
    }
  }

  void checkPaths() {
    // Create or verify the pkg, var, and tmp directories.

    // Let us be explicit about permissions for now.
    umask(0);

    // Set default paths if flags weren't provided.
    if (pkgPath == nullptr) pkgPath = kj::str("/var/sandstorm/apps/", appName);
    if (varPath == nullptr) varPath = kj::str("/var/sandstorm/grains/", grainId);

    // Check that package exists.
    KJ_SYSCALL(access(pkgPath.cStr(), R_OK | X_OK), pkgPath);

    // Create / verify existence of the var directory.  Do this as the target user.
    if (isNew) {
      if (mkdir(varPath.cStr(), 0770) != 0) {
        int error = errno;
        if (errno == EEXIST) {
          context.exitError(kj::str("Grain already exists: ", grainId));
        } else {
          KJ_FAIL_SYSCALL("mkdir(varPath.cStr(), 0770)", error, varPath);
        }
      }
      KJ_SYSCALL(mkdir(kj::str(varPath, "/sandbox").cStr(), 0770), varPath);
    } else {
      if (access(varPath.cStr(), R_OK | W_OK | X_OK) != 0) {
        int error = errno;
        if (error == ENOENT) {
          context.exitError(kj::str("No such grain: ", grainId));
        } else {
          KJ_FAIL_SYSCALL("access(varPath.cStr(), R_OK | W_OK | X_OK)", error, varPath);
        }
      }
    }

    // Create the temp directory if it doesn't exist.  We only need one tmpdir because we're just
    // going to bind it to a private mount anyway.
    if (mkdir(kj::str("/tmp/sandstorm-grain").cStr(), 0770) < 0) {
      int error = errno;
      if (error != EEXIST) {
        KJ_FAIL_SYSCALL("mkdir(\"/tmp/sandstorm-grain\")", error);
      }
    }

    // Create the log file while we're still non-superuser.
    int logfd;
    KJ_SYSCALL(logfd = open(kj::str(varPath, "/log").cStr(),
        O_WRONLY | O_APPEND | O_CLOEXEC | O_CREAT, 0600));
    KJ_SYSCALL(close(logfd));
  }

  void writeUserNSMap(const char *type, kj::StringPtr contents) {
    kj::FdOutputStream(raiiOpen(kj::str("/proc/self/", type, "_map").cStr(), O_WRONLY | O_CLOEXEC))
        .write(contents.begin(), contents.size());
  }

  void unshareOuter() {
    pid_t uid = getuid(), gid = getgid();

    // Unshare all of the namespaces except network.  Note that unsharing the pid namespace is a
    // little odd in that it doesn't actually affect this process, but affects later children
    // created by it.
    KJ_SYSCALL(unshare(CLONE_NEWUSER | CLONE_NEWNS | CLONE_NEWIPC | CLONE_NEWUTS | CLONE_NEWPID));

    // Map ourselves as 1000:1000, since it costs nothing to mask the uid and gid.
    writeUserNSMap("uid", kj::str("1000 ", uid, " 1\n"));
    writeUserNSMap("gid", kj::str("1000 ", gid, " 1\n"));

    // To really unshare the mount namespace, we also have to make sure all mounts are private.
    // The parameters here were derived by strace'ing `mount --make-rprivate /`.  AFAICT the flags
    // are undocumented.  :(
    KJ_SYSCALL(mount("none", "/", nullptr, MS_REC | MS_PRIVATE, nullptr));

    // Set a dummy host / domain so the grain can't see the real one.  (unshare(CLONE_NEWUTS) means
    // these settings only affect this process and its children.)
    KJ_SYSCALL(sethostname("sandbox", 7));
    KJ_SYSCALL(setdomainname("sandbox", 7));
  }

  void makeCharDeviceNode(const char *name, const char* realName, int major, int minor) {
    // Try with mknod first.
    auto dst = kj::str("dev/", name);
    if (mknod(dst.cStr(), S_IFCHR | 0666, makedev(major, minor)) != 0) {
      if (errno != EPERM) {
        KJ_FAIL_SYSCALL("mknod", errno, name);  // Unexpected failure
      }

      // Try a fallback: bind-mount the existing node. The overmounted file's mode doesn't matter.
      // We use mknod() to create a regular file here just because it's more direct than open()ing
      // and then close()ing.
      KJ_SYSCALL(mknod(dst.cStr(), S_IFREG | 0666, 0));
      KJ_SYSCALL(mount(kj::str("/dev/", realName).cStr(), dst.cStr(), nullptr, MS_BIND, nullptr));
    }
  }

  void setupFilesystem() {
    // The root of our mount namespace will be the app package itself.  We optionally create
    // tmp, dev, and var.  tmp is an ordinary tmpfs.  dev is a read-only tmpfs that contains
    // a few safe device nodes.  var is the 'var/sandbox' directory inside the grain.
    //
    // Now for the tricky part: the supervisor needs to be able to see a little bit more.
    // In particular, it needs to be able to see the entire var directory inside the grain.
    // We arrange for the the supervisor's special directory to be ".", even though it's
    // not mounted anywhere.

    // Set up the supervisor's directory. We immediately detach it from the mount tree, only
    // keeping a file descriptor, which we can later access via fchdir(). This prevents the
    // supervisor dir from being accessible to the app.
    bind(varPath, "/tmp/sandstorm-grain", MS_NODEV | MS_NOEXEC);
    auto supervisorDir = raiiOpen("/tmp/sandstorm-grain", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    KJ_SYSCALL(umount2("/tmp/sandstorm-grain", MNT_DETACH));

    // Bind the app package to "sandbox", which will be the grain's root directory.
    bind(pkgPath, "/tmp/sandstorm-grain", MS_NODEV | MS_RDONLY);

    // Change to that directory.
    KJ_SYSCALL(chdir("/tmp/sandstorm-grain"));

    // Optionally bind var, tmp, dev if the app requests it by having the corresponding directories
    // in the package.
    if (access("tmp", F_OK) == 0) {
      // Create a new tmpfs for this run.  We don't use a shared one or just /tmp for two reasons:
      // 1) tmpfs has no quota control, so a shared instance could be DoS'd by any one grain, or
      //    just used to effectively allocate more RAM than the grain is allowed.
      // 2) When we exit, the mount namespace disappears and the tmpfs is thus automatically
      //    unmounted.  No need for careful cleanup, and no need to implement a risky recursive
      //    delete.
      KJ_SYSCALL(mount("sandstorm-tmp", "tmp", "tmpfs", MS_NOATIME | MS_NOSUID,
                       "size=16m,nr_inodes=4k,mode=770"));
    }
    if (access("dev", F_OK) == 0) {
      KJ_SYSCALL(mount("sandstorm-dev", "dev", "tmpfs", MS_NOATIME | MS_NOSUID | MS_NOEXEC,
                       "size=1m,nr_inodes=16,mode=755"));
      makeCharDeviceNode("null", "null", 1, 3);
      makeCharDeviceNode("zero", "zero", 1, 5);
      makeCharDeviceNode("random", "urandom", 1, 9);
      makeCharDeviceNode("urandom", "urandom", 1, 9);
      KJ_SYSCALL(mount("dev", "dev", nullptr,
                       MS_REMOUNT | MS_BIND | MS_NOSUID | MS_NOEXEC | MS_NOATIME | MS_RDONLY, nullptr));
    }
    if (access("var", F_OK) == 0) {
      bind(kj::str(varPath, "/sandbox"), "var", MS_NODEV);
    }
    if (access("proc/cpuinfo", F_OK) == 0) {
      // Map in the real cpuinfo.
      bind("/proc/cpuinfo", "proc/cpuinfo", MS_NOATIME | MS_NOSUID | MS_NOEXEC | MS_NODEV);
    }

    // Grab a reference to the old root directory.
    auto oldRootDir = raiiOpen("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);

    // Keep /proc around if requested.
    if (mountProc) {
      if (access("proc", F_OK) == 0) {
        // Mount it to retain permission to mount it.  This mount will be associated with the
        // wrong pid namespce.  We'll fix it after forking.  We have to bind it: we can't mount
        // a new copy because we don't have the appropriate permission on the active pid ns.
        KJ_SYSCALL(mount("/proc", "proc", nullptr, MS_BIND | MS_REC, nullptr));
      } else {
        mountProc = false;
      }
    }


    // OK, everything is bound, so we can pivot_root.
    KJ_SYSCALL(syscall(SYS_pivot_root, "/tmp/sandstorm-grain", "/tmp/sandstorm-grain"));

    // We're now in a very strange state: our root directory is the grain directory,
    // but the old root is mounted on top of the grain directory.  As far as I can tell,
    // there is no simple way to unmount the old root, since "/" and "/." both refer to the
    // grain directory.  Fortunately, we kept a reference to the old root.
    KJ_SYSCALL(fchdir(oldRootDir));
    KJ_SYSCALL(umount2(".", MNT_DETACH));
    KJ_SYSCALL(fchdir(supervisorDir));

    // Now '.' is the grain's var and '/' is the sandbox directory.
  }

  void setupStdio() {
    // Make sure stdin is /dev/null and set stderr to go to a log file.

    if (!keepStdio) {
      // We want to replace stdin with /dev/null because even if there is no input on stdin, it
      // could inadvertently be an FD with other powers.  For example, it might be a TTY, in which
      // case you could write to it or otherwise mess with the terminal.
      int devNull;
      KJ_SYSCALL(devNull = open("/dev/null", O_RDONLY | O_CLOEXEC));
      KJ_SYSCALL(dup2(devNull, STDIN_FILENO));
      KJ_SYSCALL(close(devNull));

      // We direct stderr to a log file for debugging purposes.
      // TODO(soon):  Rotate logs.
      int log;
      KJ_SYSCALL(log = open("log", O_WRONLY | O_APPEND | O_CLOEXEC));
      KJ_SYSCALL(dup2(log, STDERR_FILENO));
      KJ_SYSCALL(close(log));
    }

    // We will later make stdout a copy of stderr specifically for the sandboxed process.  In the
    // supervisor, stdout is how we tell our parent that we're ready to receive connections.
  }

  void setupSeccomp() {
    // Install a rudimentary seccomp blacklist.
    // TODO(security): Change this to a whitelist.

    scmp_filter_ctx ctx = seccomp_init(SCMP_ACT_ALLOW);
    if (ctx == nullptr)
      KJ_FAIL_SYSCALL("seccomp_init", 0);  // No real error code
    KJ_DEFER(seccomp_release(ctx));

#define CHECK_SECCOMP(call)                     \
    do {                                        \
      if (auto result = (call)) {               \
        KJ_FAIL_SYSCALL(#call, -result);        \
      }                                         \
    } while (0)

    // Native code only for now, so there are no seccomp_arch_add calls.

    // Redundant, but this is standard and harmless.
    CHECK_SECCOMP(seccomp_attr_set(ctx, SCMP_FLTATR_CTL_NNP, 1));

    // It's easy to inadvertently issue an x32 syscall (e.g. syscall(-1)).  Such syscalls
    // should fail, but there's no need to kill the issuer.
    CHECK_SECCOMP(seccomp_attr_set(ctx, SCMP_FLTATR_ACT_BADARCH, SCMP_ACT_ERRNO(ENOSYS)));

    // Disable some things that seem scary.
    if (!devmode) {
      // ptrace is scary
      CHECK_SECCOMP(seccomp_rule_add(ctx, SCMP_ACT_ERRNO(EPERM), SCMP_SYS(ptrace), 0));
    } else {
      // Try to be somewhat safe with ptrace in dev mode.  Note that the ability to modify
      // orig_ax using ptrace allows a complete seccomp bypass.
      CHECK_SECCOMP(seccomp_rule_add(ctx, SCMP_ACT_ERRNO(EPERM), SCMP_SYS(ptrace), 1,
        SCMP_A0(SCMP_CMP_EQ, PTRACE_POKEUSER)));
      CHECK_SECCOMP(seccomp_rule_add(ctx, SCMP_ACT_ERRNO(EPERM), SCMP_SYS(ptrace), 1,
        SCMP_A0(SCMP_CMP_EQ, PTRACE_POKEUSER)));
      CHECK_SECCOMP(seccomp_rule_add(ctx, SCMP_ACT_ERRNO(EPERM), SCMP_SYS(ptrace), 1,
        SCMP_A0(SCMP_CMP_EQ, PTRACE_POKEUSER)));
      CHECK_SECCOMP(seccomp_rule_add(ctx, SCMP_ACT_ERRNO(EPERM), SCMP_SYS(ptrace), 1,
        SCMP_A0(SCMP_CMP_EQ, PTRACE_SETREGS)));
      CHECK_SECCOMP(seccomp_rule_add(ctx, SCMP_ACT_ERRNO(EPERM), SCMP_SYS(ptrace), 1,
        SCMP_A0(SCMP_CMP_EQ, PTRACE_SETFPREGS)));
      CHECK_SECCOMP(seccomp_rule_add(ctx, SCMP_ACT_ERRNO(EPERM), SCMP_SYS(ptrace), 1,
        SCMP_A0(SCMP_CMP_EQ, PTRACE_SETREGSET)));
    }

    // Restrict the set of allowable network protocol families
    CHECK_SECCOMP(seccomp_rule_add(ctx, SCMP_ACT_ERRNO(EAFNOSUPPORT), SCMP_SYS(socket), 1,
       SCMP_A0(SCMP_CMP_GE, AF_NETLINK + 1)));
    CHECK_SECCOMP(seccomp_rule_add(ctx, SCMP_ACT_ERRNO(EAFNOSUPPORT), SCMP_SYS(socket), 1,
       SCMP_A0(SCMP_CMP_EQ, AF_AX25)));
    CHECK_SECCOMP(seccomp_rule_add(ctx, SCMP_ACT_ERRNO(EAFNOSUPPORT), SCMP_SYS(socket), 1,
       SCMP_A0(SCMP_CMP_EQ, AF_IPX)));
    CHECK_SECCOMP(seccomp_rule_add(ctx, SCMP_ACT_ERRNO(EAFNOSUPPORT), SCMP_SYS(socket), 1,
       SCMP_A0(SCMP_CMP_EQ, AF_APPLETALK)));
    CHECK_SECCOMP(seccomp_rule_add(ctx, SCMP_ACT_ERRNO(EAFNOSUPPORT), SCMP_SYS(socket), 1,
       SCMP_A0(SCMP_CMP_EQ, AF_NETROM)));
    CHECK_SECCOMP(seccomp_rule_add(ctx, SCMP_ACT_ERRNO(EAFNOSUPPORT), SCMP_SYS(socket), 1,
       SCMP_A0(SCMP_CMP_EQ, AF_BRIDGE)));
    CHECK_SECCOMP(seccomp_rule_add(ctx, SCMP_ACT_ERRNO(EAFNOSUPPORT), SCMP_SYS(socket), 1,
       SCMP_A0(SCMP_CMP_EQ, AF_ATMPVC)));
    CHECK_SECCOMP(seccomp_rule_add(ctx, SCMP_ACT_ERRNO(EAFNOSUPPORT), SCMP_SYS(socket), 1,
       SCMP_A0(SCMP_CMP_EQ, AF_X25)));
    CHECK_SECCOMP(seccomp_rule_add(ctx, SCMP_ACT_ERRNO(EAFNOSUPPORT), SCMP_SYS(socket), 1,
       SCMP_A0(SCMP_CMP_EQ, AF_ROSE)));
    CHECK_SECCOMP(seccomp_rule_add(ctx, SCMP_ACT_ERRNO(EAFNOSUPPORT), SCMP_SYS(socket), 1,
       SCMP_A0(SCMP_CMP_EQ, AF_DECnet)));
    CHECK_SECCOMP(seccomp_rule_add(ctx, SCMP_ACT_ERRNO(EAFNOSUPPORT), SCMP_SYS(socket), 1,
       SCMP_A0(SCMP_CMP_EQ, AF_NETBEUI)));
    CHECK_SECCOMP(seccomp_rule_add(ctx, SCMP_ACT_ERRNO(EAFNOSUPPORT), SCMP_SYS(socket), 1,
       SCMP_A0(SCMP_CMP_EQ, AF_SECURITY)));
    CHECK_SECCOMP(seccomp_rule_add(ctx, SCMP_ACT_ERRNO(EAFNOSUPPORT), SCMP_SYS(socket), 1,
       SCMP_A0(SCMP_CMP_EQ, AF_KEY)));

    CHECK_SECCOMP(seccomp_rule_add(ctx, SCMP_ACT_ERRNO(ENOSYS), SCMP_SYS(add_key), 0));
    CHECK_SECCOMP(seccomp_rule_add(ctx, SCMP_ACT_ERRNO(ENOSYS), SCMP_SYS(request_key), 0));
    CHECK_SECCOMP(seccomp_rule_add(ctx, SCMP_ACT_ERRNO(ENOSYS), SCMP_SYS(keyctl), 0));
    CHECK_SECCOMP(seccomp_rule_add(ctx, SCMP_ACT_ERRNO(ENOSYS), SCMP_SYS(syslog), 0));
    CHECK_SECCOMP(seccomp_rule_add(ctx, SCMP_ACT_ERRNO(ENOSYS), SCMP_SYS(uselib), 0));
    CHECK_SECCOMP(seccomp_rule_add(ctx, SCMP_ACT_ERRNO(ENOSYS), SCMP_SYS(personality), 0));
    CHECK_SECCOMP(seccomp_rule_add(ctx, SCMP_ACT_ERRNO(ENOSYS), SCMP_SYS(acct), 0));

    // 16-bit code is unnecessary in the sandbox, and modify_ldt is a historic source
    // of interesting information leaks.
    CHECK_SECCOMP(seccomp_rule_add(ctx, SCMP_ACT_ERRNO(ENOSYS), SCMP_SYS(modify_ldt), 0));

    // Disable namespaces. Nested sandboxing could be useful but the attack surface is large.
    CHECK_SECCOMP(seccomp_rule_add(ctx, SCMP_ACT_ERRNO(ENOSYS), SCMP_SYS(unshare), 0));
    CHECK_SECCOMP(seccomp_rule_add(ctx, SCMP_ACT_ERRNO(ENOSYS), SCMP_SYS(mount), 0));
    CHECK_SECCOMP(seccomp_rule_add(ctx, SCMP_ACT_ERRNO(ENOSYS), SCMP_SYS(pivot_root), 0));
    CHECK_SECCOMP(seccomp_rule_add(ctx, SCMP_ACT_ERRNO(ENOSYS), SCMP_SYS(quotactl), 0));
    CHECK_SECCOMP(seccomp_rule_add(ctx, SCMP_ACT_ERRNO(EPERM), SCMP_SYS(clone), 1,
        SCMP_A0(SCMP_CMP_MASKED_EQ, CLONE_NEWUSER, CLONE_NEWUSER)));

    // AIO is scary.
    CHECK_SECCOMP(seccomp_rule_add(ctx, SCMP_ACT_ERRNO(ENOSYS), SCMP_SYS(io_setup), 0));
    CHECK_SECCOMP(seccomp_rule_add(ctx, SCMP_ACT_ERRNO(ENOSYS), SCMP_SYS(io_destroy), 0));
    CHECK_SECCOMP(seccomp_rule_add(ctx, SCMP_ACT_ERRNO(ENOSYS), SCMP_SYS(io_getevents), 0));
    CHECK_SECCOMP(seccomp_rule_add(ctx, SCMP_ACT_ERRNO(ENOSYS), SCMP_SYS(io_submit), 0));
    CHECK_SECCOMP(seccomp_rule_add(ctx, SCMP_ACT_ERRNO(ENOSYS), SCMP_SYS(io_cancel), 0));

    // Scary vm syscalls
    CHECK_SECCOMP(seccomp_rule_add(ctx, SCMP_ACT_ERRNO(ENOSYS), SCMP_SYS(remap_file_pages), 0));
    CHECK_SECCOMP(seccomp_rule_add(ctx, SCMP_ACT_ERRNO(ENOSYS), SCMP_SYS(mbind), 0));
    CHECK_SECCOMP(seccomp_rule_add(ctx, SCMP_ACT_ERRNO(ENOSYS), SCMP_SYS(get_mempolicy), 0));
    CHECK_SECCOMP(seccomp_rule_add(ctx, SCMP_ACT_ERRNO(ENOSYS), SCMP_SYS(set_mempolicy), 0));
    CHECK_SECCOMP(seccomp_rule_add(ctx, SCMP_ACT_ERRNO(ENOSYS), SCMP_SYS(migrate_pages), 0));
    CHECK_SECCOMP(seccomp_rule_add(ctx, SCMP_ACT_ERRNO(ENOSYS), SCMP_SYS(move_pages), 0));
    CHECK_SECCOMP(seccomp_rule_add(ctx, SCMP_ACT_ERRNO(ENOSYS), SCMP_SYS(vmsplice), 0));

    // Scary futex operations
    CHECK_SECCOMP(seccomp_rule_add(ctx, SCMP_ACT_ERRNO(ENOSYS), SCMP_SYS(set_robust_list), 0));
    CHECK_SECCOMP(seccomp_rule_add(ctx, SCMP_ACT_ERRNO(ENOSYS), SCMP_SYS(get_robust_list), 0));

    // Utterly terrifying profiling operations
    CHECK_SECCOMP(seccomp_rule_add(ctx, SCMP_ACT_ERRNO(ENOSYS), SCMP_SYS(perf_event_open), 0));

    // TOOD(someday): See if we can get away with turning off mincore, madvise, sysinfo etc.

    // TODO(someday): Turn off POSIX message queues and other such esoteric features.

    if (seccomp_dump_pfc)
      seccomp_export_pfc(ctx, 1);

    CHECK_SECCOMP(seccomp_load(ctx));

#undef CHECK_SECCOMP
  }

  void unshareNetwork() {
    // Unshare the network and set up a new loopback device.

    // Enter new network namespace.
    KJ_SYSCALL(unshare(CLONE_NEWNET));

    // Create a socket for our ioctls.
    int fd;
    KJ_SYSCALL(fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP));
    KJ_DEFER(close(fd));

    // Set the address of "lo".
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strcpy(ifr.ifr_ifrn.ifrn_name, "lo");
    struct sockaddr_in* addr = reinterpret_cast<struct sockaddr_in*>(&ifr.ifr_ifru.ifru_addr);
    addr->sin_family = AF_INET;
    addr->sin_addr.s_addr = htonl(0x7F000001);  // 127.0.0.1
    KJ_SYSCALL(ioctl(fd, SIOCSIFADDR, &ifr));

    // Set flags to enable "lo".
    memset(&ifr.ifr_ifru, 0, sizeof(ifr.ifr_ifru));
    ifr.ifr_ifru.ifru_flags = IFF_LOOPBACK | IFF_UP | IFF_RUNNING;
    KJ_SYSCALL(ioctl(fd, SIOCSIFFLAGS, &ifr));
  }

  void maybeFinishMountingProc() {
    // Mount proc if it was requested.  Note that this must take place after fork() to get the
    // correct pid namespace.  We must keep a copy of proc mounted at all times; otherwise we
    // lose the privilege of mounting proc.

    if (mountProc) {
      auto oldProc = raiiOpen("proc", O_RDONLY | O_DIRECTORY | O_CLOEXEC);

      // This puts the new proc onto the namespace root, which is mostly inaccessible.
      KJ_SYSCALL(mount("proc", "/", nullptr, MS_MOVE, nullptr));

      // Now mount the new proc in the right place.
      KJ_SYSCALL(mount("proc", "proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, nullptr));

      // And get rid of the old one.
      KJ_SYSCALL(fchdir(oldProc));
      KJ_SYSCALL(umount2(".", MNT_DETACH));
      KJ_SYSCALL(chdir("/"));
    }
  }

  void permanentlyDropSuperuser() {
    // Drop all Linux "capabilities".  (These are Linux/POSIX "capabilities", which are not true
    // object-capabilities, hence the quotes.)
    //
    // This unfortunately must be performed post-fork (in both parent and child), because the child
    // needs to do one final unshare().

    struct __user_cap_header_struct hdr;
    struct __user_cap_data_struct data[2];
    hdr.version = _LINUX_CAPABILITY_VERSION_3;
    hdr.pid = 0;
    memset(data, 0, sizeof(data));  // All capabilities disabled!
    KJ_SYSCALL(capset(&hdr, data));

    // Sandstorm data is private.  Don't let other users see it.  But, do grant full access to the
    // group.  The idea here is that you might have a dedicated sandstorm-sandbox user account but
    // define a special "sandstorm-admin" group which includes that account as well as a real user
    // who should have direct access to the data.
    umask(0007);
  }

  void enterSandbox() {
    // Fully enter the sandbox.  Called only by the child process.
    KJ_SYSCALL(chdir("/"));

    // Unshare the network, creating a new loopback interface.
    unshareNetwork();

    // Mount proc if --proc was passed.
    maybeFinishMountingProc();

    // Now actually drop all credentials.
    permanentlyDropSuperuser();

    // Use seccomp to disable dangerous syscalls. We do this last so that we can disable things
    // that we just used above, like unshare() or setuid().
    setupSeccomp();
  }

  // =====================================================================================

  void checkIfAlreadyRunning() {
    // Attempt to connect to any existing supervisor and call keepAlive().  If successful, we
    // don't want to start a new instance; we should use the existing instance.

    // TODO(soon):  There's a race condition if two supervisors are started up in rapid succession.
    //   We could maybe avoid that with some filesystem locking.  It's currently unlikely to happen
    //   in practice because it would require sending a request to the shell server to open the
    //   grain, then restarting the shell server, then opening the grain again, all before the
    //   first supervisor finished starting.  Or, I suppose, running two shell servers and trying
    //   to open the same grain in both at once.

    auto ioContext = kj::setupAsyncIo();

    // Connect to the client.
    auto addr = ioContext.provider->getNetwork().parseAddress("unix:socket")
        .wait(ioContext.waitScope);
    kj::Own<kj::AsyncIoStream> connection;
    KJ_IF_MAYBE(exception, kj::runCatchingExceptions([&]() {
      connection = addr->connect().wait(ioContext.waitScope);
    })) {
      // Failed to connect.  Assume socket is stale.
      return;
    }

    // Set up RPC.
    capnp::TwoPartyVatNetwork vatNetwork(*connection, capnp::rpc::twoparty::Side::CLIENT);
    auto client = capnp::makeRpcClient(vatNetwork);

    // Restore the default capability (the Supervisor interface).
    capnp::MallocMessageBuilder message;
    capnp::rpc::SturdyRef::Builder ref = message.getRoot<capnp::rpc::SturdyRef>();
    auto hostId = ref.getHostId().initAs<capnp::rpc::twoparty::SturdyRefHostId>();
    hostId.setSide(capnp::rpc::twoparty::Side::SERVER);
    Supervisor::Client cap = client.restore(hostId, ref.getObjectId()).castAs<Supervisor>();

    // Call keepAlive().
    auto promise = cap.keepAliveRequest().send();
    KJ_IF_MAYBE(exception, kj::runCatchingExceptions([&]() {
      promise.wait(ioContext.waitScope);
    })) {
      // Failed to keep-alive.  Supervisor must have died just as we were connecting to it.  Go
      // ahead and start a new one.
      return;
    }

    // We successfully connected and keepalived the existing supervisor, so we can exit.  The
    // caller is expecting us to write to stdout when the stocket is ready, so do that anyway.
    KJ_SYSCALL(write(STDOUT_FILENO, "Already running...\n", strlen("Already running...\n")));
    _exit(0);
    KJ_UNREACHABLE;
  }

  // =====================================================================================

  void runChild(int apiFd) KJ_NORETURN {
    // We are the child.

    enterSandbox();

    // Reset all signal handlers to default.  (exec() will leave ignored signals ignored, and KJ
    // code likes to ignore e.g. SIGPIPE.)
    // TODO(cleanup):  Is there a better way to do this?
    for (uint i = 0; i < NSIG; i++) {
      signal(i, SIG_DFL);  // Only possible error is EINVAL (invalid signum); we don't care.
    }

    // Unblock all signals.  (Yes, the signal mask is inherited over exec...)
    sigset_t sigmask;
    sigemptyset(&sigmask);
    KJ_SYSCALL(sigprocmask(SIG_SETMASK, &sigmask, nullptr));

    // Make sure the API socket is on FD 3.
    if (apiFd == 3) {
      // Socket end already has correct fd.  Unset CLOEXEC.
      KJ_SYSCALL(fcntl(apiFd, F_SETFD, 0));
    } else {
      // dup socket to correct fd.
      KJ_SYSCALL(dup2(apiFd, 3));
      KJ_SYSCALL(close(apiFd));
    }

    // Redirect stdout to stderr, so that our own stdout serves one purpose:  to notify the parent
    // process when we're ready to accept connections.  We previously directed stderr to a log file.
    KJ_SYSCALL(dup2(STDERR_FILENO, STDOUT_FILENO));

    char* argv[command.size() + 1];
    for (uint i: kj::indices(command)) {
      argv[i] = const_cast<char*>(command[i].cStr());
    }
    argv[command.size()] = nullptr;

    char* env[environment.size() + 1];
    for (uint i: kj::indices(environment)) {
      env[i] = const_cast<char*>(environment[i].cStr());
    }
    env[environment.size()] = nullptr;

    char** argvp = argv;  // work-around Clang not liking lambda + vararray
    char** envp = env;    // same

    KJ_SYSCALL(execve(argvp[0], argvp, envp), argvp[0]);
    KJ_UNREACHABLE;
  }

  class SandstormApiImpl final: public SandstormApi::Server {
  public:
    // TODO(someday):  Implement API.
//    kj::Promise<void> publish(PublishContext context) override {

//    }

//    kj::Promise<void> registerAction(RegisterActionContext context) override {

//    }

//    kj::Promise<void> shareCap(ShareCapContext context) override {

//    }

//    kj::Promise<void> shareView(ShareViewContext context) override {

//    }
  };

  class SupervisorImpl final: public Supervisor::Server {
  public:
    inline SupervisorImpl(UiView::Client&& mainView, DiskUsageWatcher& diskWatcher)
        : mainView(kj::mv(mainView)), diskWatcher(diskWatcher) {}

    kj::Promise<void> getMainView(GetMainViewContext context) {
      context.getResults(capnp::MessageSize {4, 1}).setView(mainView);
      return kj::READY_NOW;
    }

    kj::Promise<void> keepAlive(KeepAliveContext context) {
      sandstorm::keepAlive = true;
      return kj::READY_NOW;
    }

    kj::Promise<void> shutdown(ShutdownContext context) {
      killChildAndExit(0);
    }

    kj::Promise<void> getGrainSize(GetGrainSizeContext context) {
      context.getResults(capnp::MessageSize { 2, 0 }).setSize(diskWatcher.getSize());
      return kj::READY_NOW;
    }

    kj::Promise<void> getGrainSizeWhenDifferent(GetGrainSizeWhenDifferentContext context) {
      auto oldSize = context.getParams().getOldSize();
      context.releaseParams();
      return diskWatcher.getSizeWhenChanged(oldSize).then([context](uint64_t size) mutable {
        context.getResults(capnp::MessageSize { 2, 0 }).setSize(size);
      });
    }

  private:
    UiView::Client mainView;
    DiskUsageWatcher& diskWatcher;
  };

  class Restorer: public capnp::SturdyRefRestorer<capnp::AnyPointer> {
  public:
    explicit Restorer(capnp::Capability::Client&& defaultCap)
        : defaultCap(kj::mv(defaultCap)) {}

    capnp::Capability::Client restore(capnp::AnyPointer::Reader ref) override {
      // TODO(soon):  Make it possible to export a default capability on two-party connections.
      //   For now we use a null ref as a hack, but this is questionable because if guessable
      //   SturdyRefs exist then you can't let just any component of your system request arbitrary
      //   SturdyRefs.
      if (ref.isNull()) {
        return defaultCap;
      }

      // TODO(someday):  Implement level 2 RPC with distributed confinement.
      KJ_FAIL_ASSERT("SturdyRefs not implemented.");
    }

  private:
    capnp::Capability::Client defaultCap;
  };

  struct AcceptedConnection {
    kj::Own<kj::AsyncIoStream> connection;
    capnp::TwoPartyVatNetwork network;
    capnp::RpcSystem<capnp::rpc::twoparty::SturdyRefHostId> rpcSystem;

    explicit AcceptedConnection(Restorer& restorer, kj::Own<kj::AsyncIoStream>&& connectionParam)
        : connection(kj::mv(connectionParam)),
          network(*connection, capnp::rpc::twoparty::Side::SERVER),
          rpcSystem(capnp::makeRpcServer(network, restorer)) {}
  };

  kj::Promise<void> acceptLoop(kj::ConnectionReceiver& serverPort, Restorer& restorer,
                               kj::TaskSet& taskSet) {
    return serverPort.accept().then([&](kj::Own<kj::AsyncIoStream>&& connection) {
      auto connectionState = kj::heap<AcceptedConnection>(restorer, kj::mv(connection));
      auto promise = connectionState->network.onDisconnect();
      taskSet.add(promise.attach(kj::mv(connectionState)));
      return acceptLoop(serverPort, restorer, taskSet);
    });
  }

  class ErrorHandlerImpl: public kj::TaskSet::ErrorHandler {
  public:
    void taskFailed(kj::Exception&& exception) override {
      KJ_LOG(ERROR, "connection failed", exception);
    }
  };

  void runSupervisor(int apiFd) KJ_NORETURN {
    // We're currently in a somewhat dangerous state: our root directory is controlled
    // by the app.  If glibc reads, say, /etc/nsswitch.conf, the grain could take control
    // of the supervisor.  Fix this by chrooting to the supervisor directory.
    // TODO(someday): chroot somewhere that's guaranteed to be empty instead, so that if the
    //   supervisor storage is itself compromised it can't be used to execute arbitrary code in
    //   the supervisor process.
    KJ_SYSCALL(chroot("."));

    permanentlyDropSuperuser();
    setupSeccomp();

    // TODO(soon): Somehow make sure all grandchildren die if supervisor dies. Currently SIGKILL
    //   on the supervisor won't give it a chance to kill the sandbox pid tree. Perhaps the
    //   supervisor should actually be the app's root process? We'd have to more carefully handle
    //   SIGCHLD in that case and also worry about signals sent from the app process.

    kj::UnixEventPort::captureSignal(SIGCHLD);
    auto ioContext = kj::setupAsyncIo();

    // Detect child exit.
    auto exitPromise = ioContext.unixEventPort.onSignal(SIGCHLD).then([this](siginfo_t info) {
      KJ_ASSERT(childPid != 0);
      int status;
      KJ_SYSCALL(waitpid(childPid, &status, 0));
      childPid = 0;
      KJ_ASSERT(WIFEXITED(status) || WIFSIGNALED(status));
      if (WIFSIGNALED(status)) {
        context.exitError(kj::str(
            "** SANDSTORM SUPERVISOR: App exited due to signal ", WTERMSIG(status),
            " (", strsignal(WTERMSIG(status)), ")."));
      } else {
        context.exitError(kj::str(
            "** SANDSTORM SUPERVISOR: App exited with status code: ", WEXITSTATUS(status)));
      }
    }).eagerlyEvaluate([this](kj::Exception&& e) {
      context.exitError(kj::str(
          "** SANDSTORM SUPERVISOR: Uncaught exception waiting for child process:\n", e));
    });

    // Compute grain size and watch for changes.
    DiskUsageWatcher diskWatcher(ioContext.unixEventPort);
    auto diskWatcherTask = diskWatcher.init();

    // Set up the RPC connection to the app and export the supervisor interface.
    auto appConnection = ioContext.lowLevelProvider->wrapSocketFd(apiFd,
        kj::LowLevelAsyncIoProvider::ALREADY_CLOEXEC |
        kj::LowLevelAsyncIoProvider::TAKE_OWNERSHIP);
    capnp::TwoPartyVatNetwork appNetwork(*appConnection, capnp::rpc::twoparty::Side::SERVER);
    Restorer appRestorer(kj::heap<SandstormApiImpl>());
    auto server = capnp::makeRpcServer(appNetwork, appRestorer);

    // Get the app's UiView by restoring a null SturdyRef from it.
    capnp::MallocMessageBuilder message;
    capnp::rpc::SturdyRef::Builder ref = message.getRoot<capnp::rpc::SturdyRef>();
    auto hostId = ref.getHostId().initAs<capnp::rpc::twoparty::SturdyRefHostId>();
    hostId.setSide(capnp::rpc::twoparty::Side::CLIENT);
    UiView::Client app = server.restore(hostId, ref.getObjectId()).castAs<UiView>();

    // Set up the external RPC interface, re-exporting the UiView.
    // TODO(someday):  If there are multiple front-ends, or the front-ends restart a lot, we'll
    //   want to wrap the UiView and cache session objects.  Perhaps we could do this by making
    //   them persistable, though it's unclear how that would work with SessionContext.
    Restorer serverRestorer(kj::heap<SupervisorImpl>(kj::mv(app), diskWatcher));
    ErrorHandlerImpl errorHandler;
    kj::TaskSet tasks(errorHandler);
    unlink("socket");  // Clear stale socket, if any.
    auto acceptTask = ioContext.provider->getNetwork().parseAddress("unix:socket", 0).then(
        [&](kj::Own<kj::NetworkAddress>&& addr) {
      auto serverPort = addr->listen();
      KJ_SYSCALL(write(STDOUT_FILENO, "Listening...\n", strlen("Listening...\n")));
      auto promise = acceptLoop(*serverPort, serverRestorer, tasks);
      return promise.attach(kj::mv(serverPort));
    });

    // Wait for disconnect or accept loop failure or disk watch failure, then exit.
    acceptTask.exclusiveJoin(kj::mv(diskWatcherTask))
              .exclusiveJoin(appNetwork.onDisconnect())
              .wait(ioContext.waitScope);

    // Only onDisconnect() would return normally (rather than throw), so the app must have
    // disconnected (i.e. from the Cap'n Proto API socket).

    // Hmm, app disconnected API socket. The app probably exited and we just haven't gotten the
    // signal yet, so sleep for a moment to let it arrive, so that we can report the exit status.
    // Otherwise kill.
    ioContext.provider->getTimer().afterDelay(1 * kj::SECONDS)
        .exclusiveJoin(kj::mv(exitPromise))
        .wait(ioContext.waitScope);

    SANDSTORM_LOG("App disconnected API socket but didn't actually exit; killing it.");
    killChildAndExit(1);
  }
};

}  // namespace sandstorm

KJ_MAIN(sandstorm::SupervisorMain)
