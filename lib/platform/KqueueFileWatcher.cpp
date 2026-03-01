#include "KqueueFileWatcher.hpp"

#include <errno.h>
#include <fcntl.h>
#include <iostream>
#include <sys/event.h>
#include <sys/param.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

// O_EVTONLY: open for event notification only, without preventing the volume
// from being unmounted and without requiring read permission on the path.
#ifndef O_EVTONLY
#define O_EVTONLY O_RDONLY
#endif

// F_GETPATH: retrieve the current filesystem path of an open fd. On macOS
// this follows the inode, so after a rename it returns the new path.
#ifndef F_GETPATH
#define F_GETPATH 50
#endif

// Split "/foo/bar/baz.txt" into {"/foo/bar/", "baz.txt"}.
// Trailing slashes on the input are stripped before splitting, so
// "/foo/bar/" and "/foo/bar" both produce {"/foo/", "bar"}.
static std::pair<std::string, std::string> SplitPath(const std::string &path) {
  std::string p = path;
  while (p.size() > 1 && p.back() == '/')
    p.pop_back();
  size_t pos = p.find_last_of('/');
  if (pos == std::string::npos)
    return {"", p};
  return {p.substr(0, pos + 1), p.substr(pos + 1)};
}

// Raise the process soft fd limit to the hard limit. On macOS the hard limit
// for unprivileged processes is KERN_MAXFILESPERPROC (10240 by default), which
// is sufficient headroom for any realistic editor workload. This is a
// process-wide side effect, but the alternative — silently failing to watch
// paths once the default soft limit of 256 is reached — is worse.
static void RaiseFdLimit() {
  struct rlimit rl;
  if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < rl.rlim_max) {
    rl.rlim_cur = rl.rlim_max;
    setrlimit(RLIMIT_NOFILE, &rl);
  }
}

KqueueFileWatcher::KqueueFileWatcher() {
  RaiseFdLimit();

  kqueueFd = kqueue();
  if (kqueueFd == -1) {
    isValid = false;
    return;
  }

  // A pipe lets the destructor unblock kevent() cleanly without signals.
  if (pipe(wakeupPipe) == -1) {
    close(kqueueFd);
    kqueueFd = -1;
    isValid = false;
    return;
  }

  struct kevent ev;
  EV_SET(&ev, wakeupPipe[0], EVFILT_READ, EV_ADD, 0, 0, nullptr);
  kevent(kqueueFd, &ev, 1, nullptr, 0, nullptr);

  eventThread = std::thread(&KqueueFileWatcher::eventLoop, this);
}

KqueueFileWatcher::~KqueueFileWatcher() {
  isValid = false;
  stopping = true;

  // Unblock the event loop thread.
  char byte = 0;
  write(wakeupPipe[1], &byte, 1);

  if (eventThread.joinable()) {
    eventThread.join();
  }

  {
    std::lock_guard<std::mutex> lock(mapMutex);
    for (auto &pair : handlesToFds) {
      close(pair.second);
    }
    handlesToFds.clear();
    fdsToHandles.clear();
  }

  close(wakeupPipe[0]);
  close(wakeupPipe[1]);
  close(kqueueFd);
}

efsw::WatchID KqueueFileWatcher::addWatch(const std::string &path,
                                          efsw::FileWatchListener *listener,
                                          bool /* _useRecursion */
) {
  if (!isValid) {
    return efsw::Errors::WatcherFailed;
  }

  int fd = open(path.c_str(), O_EVTONLY);
  if (fd < 0) {
    switch (errno) {
    case ENOENT:
      return efsw::Errors::FileNotFound;
    case EACCES:
    case EPERM:
      return efsw::Errors::FileNotReadable;
    default:
      return efsw::Errors::WatcherFailed;
    }
  }

  efsw::WatchID handle;
  {
    std::lock_guard<std::mutex> lock(mapMutex);
    handle = nextHandleID++;
    handlesToFds[handle] = fd;
    fdsToHandles[fd] = handle;
    handlesToPaths[handle] = path;
    handlesToListeners[handle] = listener;
  }

  // Store the handle in udata so the event loop can identify the watch even if
  // the fd has been reused (the handle will no longer be in handlesToPaths, so
  // the event will be safely ignored).
  struct kevent ev;
  int fflags = NOTE_WRITE | NOTE_DELETE | NOTE_RENAME | NOTE_ATTRIB;
  EV_SET(&ev, fd, EVFILT_VNODE, EV_ADD | EV_ENABLE | EV_CLEAR, fflags, 0,
         reinterpret_cast<void *>(static_cast<intptr_t>(handle)));

  if (kevent(kqueueFd, &ev, 1, nullptr, 0, nullptr) == -1) {
    std::lock_guard<std::mutex> lock(mapMutex);
    handlesToFds.erase(handle);
    fdsToHandles.erase(fd);
    handlesToPaths.erase(handle);
    handlesToListeners.erase(handle);
    close(fd);
    return efsw::Errors::WatcherFailed;
  }

  return handle;
}

void KqueueFileWatcher::removeWatch(efsw::WatchID handle) {
  int fd = -1;
  {
    std::lock_guard<std::mutex> lock(mapMutex);
    auto it = handlesToFds.find(handle);
    if (it != handlesToFds.end()) {
      fd = it->second;
      handlesToFds.erase(it);
      fdsToHandles.erase(fd);
    }
    // Always erase these; the event loop may have already closed the fd
    // (e.g. after NOTE_RENAME/NOTE_DELETE) but left the handle in place.
    handlesToPaths.erase(handle);
    handlesToListeners.erase(handle);
  }
  // Closing the fd automatically removes its EVFILT_VNODE filter from the
  // kqueue.
  if (fd >= 0)
    close(fd);
}

void KqueueFileWatcher::sendFileAction(efsw::WatchID handle,
                                       const std::string &dir,
                                       const std::string &filename,
                                       efsw::Action action,
                                       const std::string &oldFilename) {
  efsw::FileWatchListener *listener = nullptr;
  {
    std::lock_guard<std::mutex> lock(mapMutex);
    auto it = handlesToListeners.find(handle);
    if (it == handlesToListeners.end())
      return;
    listener = it->second;
  }
  listener->handleFileAction(handle, dir, filename, action, oldFilename);
}

void KqueueFileWatcher::closeFd(efsw::WatchID handle, int fd) {
  {
    std::lock_guard<std::mutex> lock(mapMutex);
    handlesToFds.erase(handle);
    fdsToHandles.erase(fd);
  }
  close(fd);
}

bool KqueueFileWatcher::reopenFd(efsw::WatchID handle,
                                 const std::string &path) {
  int newFd = open(path.c_str(), O_EVTONLY);
  if (newFd < 0)
    return false;

  {
    std::lock_guard<std::mutex> lock(mapMutex);
    handlesToFds[handle] = newFd;
    fdsToHandles[newFd] = handle;
  }

  struct kevent ev;
  int fflags = NOTE_WRITE | NOTE_DELETE | NOTE_RENAME | NOTE_ATTRIB;
  EV_SET(&ev, newFd, EVFILT_VNODE, EV_ADD | EV_ENABLE | EV_CLEAR, fflags, 0,
         reinterpret_cast<void *>(static_cast<intptr_t>(handle)));

  if (kevent(kqueueFd, &ev, 1, nullptr, 0, nullptr) == -1) {
    std::lock_guard<std::mutex> lock(mapMutex);
    handlesToFds.erase(handle);
    fdsToHandles.erase(newFd);
    close(newFd);
    return false;
  }

  return true;
}

void KqueueFileWatcher::eventLoop() {
  while (!stopping) {
    struct kevent event;
    int r;
    do {
      r = kevent(kqueueFd, nullptr, 0, &event, 1, nullptr);
    } while (r == -1 && errno == EINTR);

    if (r <= 0 || stopping)
      break;

    // Wakeup pipe: destructor is signalling us to exit.
    if (static_cast<int>(event.ident) == wakeupPipe[0])
      break;

    int fd = static_cast<int>(event.ident);

    // Recover the handle from the udata we stored at registration time.
    // This avoids a map lookup on the fd, which could be stale if the fd
    // was closed and its number reused by the OS.
    efsw::WatchID handle =
        static_cast<efsw::WatchID>(reinterpret_cast<intptr_t>(event.udata));

    // Confirm the handle is still registered. If removeWatch() was called
    // between the event being queued and us processing it, skip.
    std::string watchedPath;
    {
      std::lock_guard<std::mutex> lock(mapMutex);
      auto it = handlesToPaths.find(handle);
      if (it == handlesToPaths.end())
        continue;
      watchedPath = it->second;
    }

    std::pair<std::string, std::string> parts = SplitPath(watchedPath);
    const std::string &dir = parts.first;
    const std::string &filename = parts.second;

    if (event.fflags & NOTE_RENAME) {
      // The inode we were watching has been renamed. F_GETPATH returns its
      // current (new) path. We must call it before closing the fd.
      char newPathBuf[MAXPATHLEN] = {0};
      bool gotPath = (fcntl(fd, F_GETPATH, newPathBuf) == 0);
      std::string newPath(newPathBuf);

      closeFd(handle, fd);

      if (gotPath && !newPath.empty() && newPath != watchedPath) {
        // The file moved to a genuinely different path. Report a move, using
        // the old basename as context. The handle is now effectively without
        // an active fd; the caller should removeWatch() and addWatch() again
        // at the new path if it wants to keep following it.
        std::pair<std::string, std::string> newParts = SplitPath(newPath);
        if (parts.first != newParts.first) {
          // Treat moves outside of the directory as deletions.
          sendFileAction(handle, dir, filename, efsw::Actions::Delete);
        } else {
          sendFileAction(handle, newParts.first, newParts.second,
                         efsw::Actions::Moved, filename);
        }
      } else {
        // F_GETPATH failed or returned the same path — treat as a deletion.
        sendFileAction(handle, dir, filename, efsw::Actions::Delete);
      }

    } else if (event.fflags & NOTE_DELETE) {
      // The inode was deleted. This also fires when an atomic save replaces
      // the file: rename(tmp, target) unlinks target's inode, then places
      // tmp's inode at target's path. Because rename(2) is atomic, by the
      // time kevent delivers NOTE_DELETE the new file is already in place.
      closeFd(handle, fd);

      struct stat st;
      if (stat(watchedPath.c_str(), &st) == 0 &&
          reopenFd(handle, watchedPath)) {
        // A new file appeared at the same path: atomic save.
        sendFileAction(handle, dir, filename, efsw::Actions::Modified);
      } else {
        // The path is genuinely gone.
        sendFileAction(handle, dir, filename, efsw::Actions::Delete);
      }

    } else if (event.fflags & NOTE_WRITE) {
      sendFileAction(handle, dir, filename, efsw::Actions::Modified);
    } else if (event.fflags & NOTE_ATTRIB) {
      // macOS sometimes skips NOTE_WRITE when a file is truncated to empty,
      // firing NOTE_ATTRIB instead. Detect this by seeking to the end.
      if (lseek(fd, 0, SEEK_END) == 0) {
        sendFileAction(handle, dir, filename, efsw::Actions::Modified);
      }
    }
  }
}
