
#include "InotifyFileWatcher.hpp"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <string.h>
#include <sys/inotify.h>
#include <unistd.h>

#ifdef DEBUG
#include <iostream>
#endif

namespace {

// IN_CREATE/IN_DELETE/IN_MOVED_*: children appearing, disappearing, or being
// renamed within the watched directory.
// IN_MODIFY/IN_CLOSE_WRITE: content changes to children.
// IN_DELETE_SELF/IN_MOVE_SELF: the watched directory itself goes away.
constexpr uint32_t kWatchMask = IN_CREATE | IN_DELETE | IN_DELETE_SELF |
                                IN_MOVE_SELF | IN_MOVED_FROM | IN_MOVED_TO |
                                IN_MODIFY | IN_CLOSE_WRITE;

constexpr size_t kEventBufLen =
    64 * (sizeof(struct inotify_event) + NAME_MAX + 1);

} // namespace

InotifyFileWatcher::InotifyFileWatcher() {
  inotifyFd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
  if (inotifyFd == -1) {
    isValid = false;
    return;
  }

  // A pipe lets the destructor unblock poll() cleanly without signals.
  if (pipe(wakeupPipe) == -1) {
    close(inotifyFd);
    inotifyFd = -1;
    isValid = false;
    return;
  }

  eventThread = std::thread(&InotifyFileWatcher::eventLoop, this);
}

InotifyFileWatcher::~InotifyFileWatcher() {
  isValid = false;
  stopping = true;

  // Unblock the event loop thread.
  char byte = 0;
  write(wakeupPipe[1], &byte, 1);

  if (eventThread.joinable())
    eventThread.join();

  close(wakeupPipe[0]);
  close(wakeupPipe[1]);
  // Closing the inotify fd automatically removes all of its watches.
  if (inotifyFd >= 0)
    close(inotifyFd);
}

efsw::WatchID InotifyFileWatcher::addWatch(const std::string &path,
                                           efsw::FileWatchListener *listener,
                                           bool /* _useRecursion */) {
#ifdef DEBUG
  std::cout << "InotifyFileWatcher::addWatch: " << path << std::endl;
#endif
  if (!isValid)
    return efsw::Errors::WatcherFailed;

  std::string dir = path;
  if (dir.empty() || dir.back() != '/')
    dir += '/';

  int wd = inotify_add_watch(inotifyFd, dir.c_str(), kWatchMask);
  if (wd < 0) {
    switch (errno) {
    case ENOENT:
      return efsw::Errors::FileNotFound;
    case EACCES:
      return efsw::Errors::FileNotReadable;
    default:
      return efsw::Errors::WatcherFailed;
    }
  }

  std::lock_guard<std::mutex> lock(mapMutex);
  efsw::WatchID handle = nextHandleID++;
  handlesToWatches[handle] = {dir, listener, wd};
  wdToHandles.insert({wd, handle});
  return handle;
}

void InotifyFileWatcher::removeWatch(efsw::WatchID handle) {
  int wd = -1;
  bool wasLastHandleForWd = false;
  {
    std::lock_guard<std::mutex> lock(mapMutex);
    auto it = handlesToWatches.find(handle);
    if (it == handlesToWatches.end())
      return;
    wd = it->second.wd;
    handlesToWatches.erase(it);

    auto range = wdToHandles.equal_range(wd);
    for (auto wit = range.first; wit != range.second; ++wit) {
      if (wit->second == handle) {
        wdToHandles.erase(wit);
        break;
      }
    }
    wasLastHandleForWd = (wdToHandles.find(wd) == wdToHandles.end());
  }

  if (wasLastHandleForWd) {
    // EINVAL here just means the kernel already removed this watch (e.g. the
    // directory was deleted); safe to ignore.
    inotify_rm_watch(inotifyFd, wd);
  }
}

void InotifyFileWatcher::forgetWatch(int wd) {
  std::lock_guard<std::mutex> lock(mapMutex);
  auto range = wdToHandles.equal_range(wd);
  for (auto it = range.first; it != range.second;) {
    handlesToWatches.erase(it->second);
    it = wdToHandles.erase(it);
  }
}

void InotifyFileWatcher::sendFileAction(int wd, const std::string &filename,
                                        efsw::Action action,
                                        const std::string &oldFilename) {
  // Copy out (handle, dir, listener) under the lock, then call into listener
  // code without holding it.
  std::vector<std::tuple<efsw::WatchID, std::string, efsw::FileWatchListener *>>
      targets;
  {
    std::lock_guard<std::mutex> lock(mapMutex);
    auto range = wdToHandles.equal_range(wd);
    for (auto it = range.first; it != range.second; ++it) {
      auto hit = handlesToWatches.find(it->second);
      if (hit != handlesToWatches.end())
        targets.emplace_back(it->second, hit->second.dir, hit->second.listener);
    }
  }

  for (auto &[handle, dir, listener] : targets)
    listener->handleFileAction(handle, dir, filename, action, oldFilename);
}

void InotifyFileWatcher::eventLoop() {
  char buf[kEventBufLen];

  // State for pairing IN_MOVED_FROM with a following IN_MOVED_TO that shares
  // its cookie and watch — i.e. a rename within the same directory.
  bool hasPendingMove = false;
  int pendingWd = -1;
  uint32_t pendingCookie = 0;
  std::string pendingFilename;

  auto flushPendingMove = [&]() {
    if (!hasPendingMove)
      return;
    // No matching IN_MOVED_TO arrived: the file was moved somewhere we're not
    // watching (or out of the filesystem entirely).
    sendFileAction(pendingWd, pendingFilename, efsw::Actions::Delete);
    hasPendingMove = false;
  };

  while (!stopping) {
    struct pollfd fds[2];
    fds[0] = {inotifyFd, POLLIN, 0};
    fds[1] = {wakeupPipe[0], POLLIN, 0};

    // If we're sitting on an unpaired IN_MOVED_FROM, don't block forever —
    // give its IN_MOVED_TO a brief window to show up, then resolve it.
    int timeoutMs = hasPendingMove ? 10 : -1;

    int r = poll(fds, 2, timeoutMs);
    if (r < 0) {
      if (errno == EINTR)
        continue;
      break;
    }

    if (stopping || (fds[1].revents & POLLIN))
      break;

    if (r == 0) {
      flushPendingMove();
      continue;
    }

    if (!(fds[0].revents & POLLIN))
      continue;

    ssize_t len = read(inotifyFd, buf, sizeof(buf));
    if (len <= 0)
      continue;

    ssize_t i = 0;
    while (i < len) {
      auto *event = reinterpret_cast<struct inotify_event *>(&buf[i]);
      i += sizeof(struct inotify_event) + event->len;

      if (event->mask & IN_Q_OVERFLOW)
        continue;

      std::string filename = event->len > 0 ? std::string(event->name) : "";

      if (hasPendingMove) {
        if ((event->mask & IN_MOVED_TO) && event->wd == pendingWd &&
            event->cookie == pendingCookie) {
          // Same-directory rename — almost always an atomic save.
          sendFileAction(event->wd, filename, efsw::Actions::Moved,
                         pendingFilename);
          hasPendingMove = false;
          continue;
        }
        flushPendingMove();
        // fall through and process this event normally
      }

      if (event->mask & IN_IGNORED) {
        // The kernel already removed this watch (explicit removeWatch,
        // directory deleted, or filesystem unmounted).
        forgetWatch(event->wd);
        continue;
      }

      if (event->mask & IN_MOVED_FROM) {
        hasPendingMove = true;
        pendingWd = event->wd;
        pendingCookie = event->cookie;
        pendingFilename = filename;
        continue;
      }

      if (event->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) {
        // The watched directory itself is gone or has moved. Report it as a
        // deletion of the directory; the caller can re-addWatch if it cares
        // to keep following it (we have no way to learn its new path).
        sendFileAction(event->wd, "", efsw::Actions::Delete);
        continue;
      }

      if (event->mask & IN_CREATE) {
        sendFileAction(event->wd, filename, efsw::Actions::Add);
      } else if (event->mask & IN_DELETE) {
        sendFileAction(event->wd, filename, efsw::Actions::Delete);
      } else if (event->mask & IN_MOVED_TO) {
        // Arrived from outside this directory (or its IN_MOVED_FROM partner
        // already timed out): treat it as a brand-new file.
        sendFileAction(event->wd, filename, efsw::Actions::Add);
        sendFileAction(event->wd, filename, efsw::Actions::Modified);
      } else if (event->mask & (IN_MODIFY | IN_CLOSE_WRITE)) {
        sendFileAction(event->wd, filename, efsw::Actions::Modified);
      }
    }
  }

  flushPendingMove();
}
