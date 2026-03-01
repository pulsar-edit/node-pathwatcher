#pragma once

#include <atomic>
#include <mutex>
#include <thread>
#include <string>
#include <unordered_map>
#include "../../vendor/efsw/include/efsw/efsw.hpp"

// An API-compatible replacement for FSEventsFileWatcher that uses kqueue
// instead of FSEvents. Intended for experimentation; swap in via core.h.
//
// Key differences from FSEventsFileWatcher:
// - No daemon dependency (no fseventsd); pure kernel interface.
// - Consumes one file descriptor per watched path (using O_EVTONLY).
// - Raises the process soft fd limit to the hard limit on construction.
// - Watches inode identity, not path identity: when a file is renamed, the
//   fd follows the inode. Atomic saves (which replace the inode) are detected
//   via stat() after NOTE_DELETE and reported as Modified rather than Delete.
// - No recursive watching; the _useRecursion flag is ignored.
class KqueueFileWatcher {
public:
  KqueueFileWatcher();
  ~KqueueFileWatcher();

  efsw::WatchID addWatch(
    const std::string& path,
    efsw::FileWatchListener* listener,
    bool _useRecursion = false
  );

  void removeWatch(efsw::WatchID handle);

  bool isValid = true;

private:
  void eventLoop();

  void sendFileAction(
    efsw::WatchID handle,
    const std::string& dir,
    const std::string& filename,
    efsw::Action action,
    const std::string& oldFilename = ""
  );

  // Closes the fd and removes it from the fd maps, but leaves the handle in
  // handlesToPaths / handlesToListeners so that removeWatch() still works.
  void closeFd(efsw::WatchID handle, int fd);

  // Opens a new fd for the given path and registers it with kqueue under an
  // existing handle. Used after an atomic save replaces the watched inode.
  bool reopenFd(efsw::WatchID handle, const std::string& path);

  long nextHandleID = 1;
  int kqueueFd = -1;
  int wakeupPipe[2] = {-1, -1};
  std::atomic<bool> stopping{false};
  std::mutex mapMutex;
  std::thread eventThread;

  std::unordered_map<efsw::WatchID, int>                      handlesToFds;
  std::unordered_map<int, efsw::WatchID>                      fdsToHandles;
  std::unordered_map<efsw::WatchID, std::string>              handlesToPaths;
  std::unordered_map<efsw::WatchID, efsw::FileWatchListener*> handlesToListeners;
};
