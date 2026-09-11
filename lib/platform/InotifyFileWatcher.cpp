
#include "InotifyFileWatcher.hpp"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <string.h>
#include <sys/inotify.h>
#include <tuple>
#include <unistd.h>
#include <vector>

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

  // Unblock the event loop thread. Nothing useful to do if this fails, but
  // `write` is declared `warn_unused_result`, so discard the result
  // explicitly rather than let it warn.
  char byte = 0;
  (void)write(wakeupPipe[1], &byte, 1);

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

  // We hold `mapMutex` across the `inotify_add_watch()` call below, just as
  // `removeWatch()` does for its removal. The kernel can start queueing events
  // for this watch the moment it exists, and the event loop needs the same
  // lock to find the listeners for a wd — so taking it first means the event
  // loop waits for us to finish registering rather than reading events for a
  // wd we haven't recorded yet and dropping them on the floor.
  std::lock_guard<std::mutex> lock(mapMutex);

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

  efsw::WatchID handle = nextHandleID++;
  handlesToWatches[handle] = {dir, listener, wd};
  wdToHandles.insert({wd, handle});
  return handle;
}

void InotifyFileWatcher::removeWatch(efsw::WatchID handle) {
  // We hold `mapMutex` across the `inotify_rm_watch()` call below. That's
  // load-bearing: it ensures we've recorded that we're expecting an
  // IN_IGNORED before the event loop — which takes the same lock in
  // `forgetWatch()` — can possibly read it.
  std::lock_guard<std::mutex> lock(mapMutex);
  auto it = handlesToWatches.find(handle);
  if (it == handlesToWatches.end())
    return;
  int wd = it->second.wd;
  handlesToWatches.erase(it);

  auto range = wdToHandles.equal_range(wd);
  for (auto wit = range.first; wit != range.second; ++wit) {
    if (wit->second == handle) {
      wdToHandles.erase(wit);
      break;
    }
  }

  // Other handles still care about this wd, so leave the kernel watch alone.
  if (wdToHandles.find(wd) != wdToHandles.end())
    return;

  // Removing the watch makes the kernel queue an IN_IGNORED for this wd. Note
  // that we're expecting it, so the event loop doesn't mistake it for a
  // kernel-initiated removal of whatever watch holds this number by the time
  // we get around to reading it. EINVAL means the kernel already dropped the
  // watch on its own (e.g. the directory was deleted), in which case nothing
  // new is coming from us and there's nothing to account for.
  if (inotify_rm_watch(inotifyFd, wd) == 0)
    pendingIgnored[wd]++;
}

void InotifyFileWatcher::forgetWatch(int wd) {
  std::lock_guard<std::mutex> lock(mapMutex);

  auto pending = pendingIgnored.find(wd);
  if (pending != pendingIgnored.end()) {
    // This IN_IGNORED is the echo of our own `inotify_rm_watch()`;
    // `removeWatch()` already dropped the bookkeeping. Anything registered
    // under this wd now is a newer watch that reuses the number, so we leave
    // it alone.
    if (--pending->second <= 0)
      pendingIgnored.erase(pending);
    return;
  }

  auto range = wdToHandles.equal_range(wd);
  for (auto it = range.first; it != range.second;) {
    handlesToWatches.erase(it->second);
    it = wdToHandles.erase(it);
  }
}

void InotifyFileWatcher::stopWatch(int wd) {
  std::lock_guard<std::mutex> lock(mapMutex);

  auto range = wdToHandles.equal_range(wd);
  for (auto it = range.first; it != range.second;) {
    handlesToWatches.erase(it->second);
    it = wdToHandles.erase(it);
  }

  // Same accounting as `removeWatch()`: the kernel queues an IN_IGNORED for
  // every watch we remove, and `forgetWatch()` needs to know we asked for it.
  // A failure here means the watch was already gone, so nothing is coming.
  if (inotify_rm_watch(inotifyFd, wd) == 0)
    pendingIgnored[wd]++;
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
  // `alignas` because we cast into this buffer to read `inotify_event`s out of
  // it, and that type has stricter alignment than `char`.
  alignas(struct inotify_event) char buf[kEventBufLen];

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
      // Any other poll() failure is not something we can recover from.
      isValid = false;
      break;
    }

    if (stopping || (fds[1].revents & POLLIN))
      break;

    // An error on either descriptor is fatal, and we must not `continue` past
    // it: poll() reports the condition immediately and forever, ignoring our
    // timeout, so looping would spin at 100% CPU without ever reading
    // anything. Better to stop the thread and mark ourselves invalid, which
    // makes subsequent `addWatch()` calls fail loudly instead of silently
    // never delivering events.
    if ((fds[0].revents | fds[1].revents) & (POLLERR | POLLHUP | POLLNVAL)) {
      isValid = false;
      break;
    }

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

      if (event->mask & IN_Q_OVERFLOW) {
        // The kernel's event queue filled up and it dropped events to make
        // room. There's nothing we can do to recover them, and no way to know
        // what we missed, so every watch is potentially out of sync from here
        // on. Worth logging, since it's otherwise invisible and would look
        // like the watcher simply stopped noticing some edits.
#ifdef DEBUG
        std::cout << "InotifyFileWatcher: event queue overflowed; some events "
                     "were dropped by the kernel"
                  << std::endl;
#endif
        continue;
      }

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
        // The watch is gone: either we removed it ourselves, or the kernel
        // dropped it (directory deleted, filesystem unmounted).
        // `forgetWatch` tells those two cases apart.
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
        //
        // This must happen before any teardown below, since `sendFileAction`
        // finds its listeners by looking up `wd` in our bookkeeping.
        sendFileAction(event->wd, "", efsw::Actions::Delete);

        if (event->mask & IN_MOVE_SELF) {
          // Unlike a deletion, a move leaves the watch alive — the directory
          // still exists, just somewhere else — and the kernel sends no
          // IN_IGNORED. Left alone, it would keep reporting events for the
          // directory's children under the stale path we recorded in
          // `Watch::dir`. We've just told the caller the directory is gone,
          // so make that true and shut the watch down.
          stopWatch(event->wd);
        }
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
