#pragma once

#include "../../vendor/efsw/include/efsw/efsw.hpp"
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// An API-compatible replacement for `efsw::FileWatcher` that talks to
// `ReadDirectoryChangesW` directly. Plays the same role on Windows that
// `InotifyFileWatcher` plays on Linux.
//
// Key differences from `FileWatcherWin32`:
//
// * Ownership of in-flight I/O. Every live watch has a
//   `ReadDirectoryChangesW` outstanding, and the kernel writes into that
//   watch's `OVERLAPPED` and buffer when the operation completes — including
//   when it completes because we cancelled it. `CancelIoEx` doesn't wait for
//   that to happen. So `removeWatch()` never frees a watch with an operation
//   in flight: it cancels the operation and leaves the watch for the
//   completion thread, which frees it once it dequeues the final completion
//   packet. (efsw freed it immediately, and the kernel's write of
//   `STATUS_CANCELLED` would land in whatever had since been allocated there.)
// * No recursion: the `_useRecursion` flag is ignored, and only the directory
//   passed to `addWatch()` is watched. The JS layer only ever watches a
//   directory — either the one it cares about, or the parent of a watched
//   file.
// * Renames within the watched directory arrive as a `RENAMED_OLD_NAME` /
//   `RENAMED_NEW_NAME` pair and are reported as `Moved`. A `RENAMED_NEW_NAME`
//   with no partner (something moved in from elsewhere) is reported as `Add`;
//   something moved out of the directory arrives as `REMOVED`. This matches
//   what efsw reported.
// * If the watched directory is renamed or moved, the watch stops quietly:
//   every path it reported would be built from the directory's old location.
//   If the directory is deleted, the watch likewise goes dormant. Either way,
//   the caller still owns the handle and should `removeWatch()` it.
//
class ReadDirectoryChangesFileWatcher {
public:
  ReadDirectoryChangesFileWatcher();
  ~ReadDirectoryChangesFileWatcher();

  efsw::WatchID addWatch(const std::string &path,
                         efsw::FileWatchListener *listener,
                         bool _useRecursion = false);

  void removeWatch(efsw::WatchID handle);

  // Atomic because the completion thread clears it if the completion port
  // fails, while `addWatch()` reads it on the main thread.
  std::atomic<bool> isValid{true};

private:
  // Defined in the .cpp file so that this header needn't include
  // `<windows.h>`. (`core.cc` includes libuv — and thus `winsock2.h` — after
  // this header, and `<windows.h>` must not get there first.)
  struct Watch;
  struct Event;

  void completionLoop();

  // Handles a completion packet for `watch`, re-arming it if appropriate.
  // Appends any file actions to `events` rather than delivering them, since
  // listeners must be called without `mapMutex` held. Called on the
  // completion thread with `mapMutex` held.
  void handleCompletion(Watch *watch, bool succeeded, unsigned long bytes,
                        unsigned long error, std::vector<Event> &events);

  // Issues a new `ReadDirectoryChangesW` for `watch`. Returns `false` if it
  // couldn't be issued, in which case `watch` has no operation in flight.
  // Caller must hold `mapMutex`.
  bool arm(Watch *watch);

  // Closes `watch`'s directory handle and marks it as having no operation in
  // flight; the watch stays registered but will never report again. Caller
  // must hold `mapMutex`, and no operation may be in flight.
  void goDormant(Watch *watch);

  // Stops `watch` and gives up our reference to it. If it has an operation in
  // flight, the operation is cancelled and the completion thread frees the
  // watch once the final packet arrives; otherwise the watch is freed now.
  // Caller must hold `mapMutex`, and must already have removed `watch` from
  // `handlesToWatches`.
  void retire(Watch *watch);

  // Closes the directory handle (if still open) and deletes `watch`.
  static void destroy(Watch *watch);

  long nextHandleID = 1;
  void *iocp = nullptr;
  bool stopping = false;
  std::mutex mapMutex;
  std::thread completionThread;

  std::unordered_map<efsw::WatchID, Watch *> handlesToWatches;

  // Watches that have been removed but still have an operation in flight.
  // The completion thread deletes each one when its final packet arrives.
  std::unordered_set<Watch *> retiredWatches;

  // The number of `ReadDirectoryChangesW` operations that have been issued
  // but whose completion packets haven't yet been dequeued. The completion
  // thread won't exit until this reaches zero, since exiting any earlier
  // would leave the kernel with pointers into memory we can no longer free
  // safely.
  size_t operationsInFlight = 0;
};
