#pragma once

#include "../watcher.h"
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

// A watcher that talks to inotify directly. Plays the same role on Linux that
// `KqueueFileWatcher` and `FSEventsFileWatcher` play on macOS. See `watcher.h`
// for the interface.
//
// Key differences from efsw’s `FileWatcherInotify`, which this replaced:
//
// * A single `inotify` instance backs every watch; `addWatch()` just calls
//   `inotify_add_watch()` against it and gets back a watch descriptor (wd). No
//   per-path file descriptors, and no fd-limit juggling.
// * No recursion: the `_useRecursion` flag is ignored, and only the directory
//   passed to addWatch() is watched. The existing JS layer only ever calls
//   `addWatch()` with a directory path — either the directory being watched
//   directly, or the parent of a watched file — so this matches how the old
//   inotify backend was used in practice anyway.
// * Renames are reconstructed from `IN_MOVED_FROM`/`IN_MOVED_TO` pairs that
//   share a cookie and land on the same watch. This is how editors' atomic
//   saves (write tmp file, rename over target) show up, and it's reported as
//   `Moved`, which the existing JS rename-handling collapses into a `change`
//   event for the watched file. Cross-directory moves are reported as a Delete
//   (on the source watch) plus an Add+Modified (on the destination watch),
//   matching the old inotify-based behavior.
// * If the watched directory itself is removed or renamed (`IN_DELETE_SELF` /
//   `IN_MOVE_SELF`), we report a single Delete for the directory. `inotify`
//   gives us no way to recover the new path of a moved directory, so the
//   caller must re-`addWatch` if it wants to keep following it.
//
class InotifyFileWatcher {
public:
  InotifyFileWatcher();
  ~InotifyFileWatcher();

  pathwatcher::WatchID addWatch(const std::string &path,
                         pathwatcher::FileWatchListener *listener,
                         bool _useRecursion = false);

  void removeWatch(pathwatcher::WatchID handle);

  // Atomic because the event loop thread clears it when it hits an
  // unrecoverable poll() error, while `addWatch()` reads it on the main
  // thread.
  std::atomic<bool> isValid{true};

private:
  struct Watch {
    std::string dir; // always ends with '/'
    pathwatcher::FileWatchListener *listener;
    int wd;
  };

  void eventLoop();

  // Looks up every handle registered for `wd` and dispatches `action` to each
  // of their listeners.
  void sendFileAction(int wd, const std::string &filename, pathwatcher::Action action,
                      const std::string &oldFilename = "");

  // Responds to an IN_IGNORED for `wd`. If we provoked it ourselves by
  // calling `inotify_rm_watch()`, it's already accounted for and we do
  // nothing. Otherwise the kernel dropped the watch on its own (e.g. the
  // directory was deleted or unmounted), so we drop all bookkeeping for `wd`.
  void forgetWatch(int wd);

  // Tears down a watch that the kernel is still holding open, dropping all
  // bookkeeping for `wd` and telling the kernel to stop watching. Used when
  // the watched directory moves (IN_MOVE_SELF): the watch survives the move,
  // but it would report every subsequent event under the directory's old,
  // now-wrong path, so we shut it down instead.
  void stopWatch(int wd);

  long nextHandleID = 1;
  int inotifyFd = -1;
  int wakeupPipe[2] = {-1, -1};
  std::atomic<bool> stopping{false};
  std::mutex mapMutex;
  std::thread eventThread;

  std::unordered_map<pathwatcher::WatchID, Watch> handlesToWatches;
  std::unordered_multimap<int, pathwatcher::WatchID> wdToHandles;

  // wd -> number of IN_IGNORED events we've asked the kernel for (via
  // `inotify_rm_watch()`) but haven't read yet. Lets the event loop tell our
  // own removals apart from kernel-initiated ones.
  std::unordered_map<int, int> pendingIgnored;
};
