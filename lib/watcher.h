#pragma once

#include <string>

// Types shared between `core.cc` and the per-platform watcher implementations
// in `platform/`. Each platform defines a class with this interface:
//
//   WatchID addWatch(const std::string &path, FileWatchListener *listener,
//                    bool _useRecursion = false);
//   void removeWatch(WatchID handle);
//
// …and `core.h` picks one of them as `FileWatcher`.
//
// This interface originally came from efsw
// (https://github.com/SpartanJ/efsw), which this library used to vendor.
namespace pathwatcher {

// Identifies a watch. `addWatch()` returns a positive `WatchID` on success,
// or one of the negative `Errors::Error` values on failure.
typedef long WatchID;

namespace Actions {
enum Action {
  // A file was created, or renamed into the watched directory.
  Add = 1,
  // A file was deleted, or renamed out of the watched directory.
  Delete = 2,
  // A file's contents changed.
  Modified = 3,
  // A file was renamed within the watched directory.
  Moved = 4
};
}
typedef Actions::Action Action;

// These values reach JavaScript as the `code` of the error thrown when a
// watch can't be added, so they shouldn't change.
namespace Errors {
enum Error {
  NoError = 0,
  FileNotFound = -1,
  FileRepeated = -2,
  FileOutOfScope = -3,
  FileNotReadable = -4,
  // The directory is on a remote file system.
  FileRemote = -5,
  // The watcher failed to watch for changes.
  WatcherFailed = -6,
  Unspecified = -7
};
}

// Receives file actions from a watcher. Called on the watcher's own thread.
class FileWatchListener {
public:
  virtual ~FileWatchListener() {}

  // `dir` is the watched directory, with a trailing path separator;
  // `filename` is relative to it. For `Moved`, `oldFilename` is the file's
  // previous name, also relative to `dir`.
  virtual void handleFileAction(WatchID watchid, const std::string &dir,
                                const std::string &filename, Action action,
                                std::string oldFilename = "") = 0;
};

} // namespace pathwatcher
