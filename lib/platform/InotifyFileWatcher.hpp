#pragma once

#include "../../vendor/efsw/include/efsw/efsw.hpp"
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

// An API-compatible replacement for `efsw::FileWatcher` that talks to inotify
// directly. Plays the same role on Linux that `KqueueFileWatcher` and
// `FSEventsFileWatcher` play on macOS.
//
// Key differences from `FileWatcherInotify`:
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

  efsw::WatchID addWatch(const std::string &path,
                         efsw::FileWatchListener *listener,
                         bool _useRecursion = false);

  void removeWatch(efsw::WatchID handle);

  bool isValid = true;

private:
  struct Watch {
    std::string dir; // always ends with '/'
    efsw::FileWatchListener *listener;
    int wd;
  };

  void eventLoop();

  // Looks up every handle registered for `wd` and dispatches `action` to each
  // of their listeners.
  void sendFileAction(int wd, const std::string &filename, efsw::Action action,
                      const std::string &oldFilename = "");

  // Drops all bookkeeping for `wd` without calling `inotify_rm_watch()`; used
  // when the kernel tells us (via IN_IGNORED) that it already dropped the
  // watch on its own (e.g. the directory was deleted or unmounted).
  void forgetWatch(int wd);

  long nextHandleID = 1;
  int inotifyFd = -1;
  int wakeupPipe[2] = {-1, -1};
  std::atomic<bool> stopping{false};
  std::mutex mapMutex;
  std::thread eventThread;

  std::unordered_map<efsw::WatchID, Watch> handlesToWatches;
  std::unordered_multimap<int, efsw::WatchID> wdToHandles;
};
