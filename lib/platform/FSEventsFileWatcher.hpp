#pragma once

#include <unordered_map>
#include <string>
#include <set>
#include <vector>
#include <mutex>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreServices/CoreServices.h>
#include "../../vendor/efsw/include/efsw/efsw.hpp"

class FSEvent {
public:
  FSEvent(
    std::string path,
    long flags,
    uint64_t id,
    uint64_t inode = 0
  ): path(path), flags(flags), id(id), inode(inode) {
#ifdef DEBUG
  std::cout << "[creating] FSEventsFileWatcher!" << std::endl;
#endif
  }

  std::string path;
  long flags;
  uint64_t id;
  uint64_t inode;
};

class FSEventsFileWatcher {
public:
  FSEventsFileWatcher() {};
  ~FSEventsFileWatcher();
  efsw::WatchID addWatch(
    const std::string& directory,
    efsw::FileWatchListener* watcher,
    bool _useRecursion = false
  );
  void removeWatch(
    efsw::WatchID watchID
  );

  void handleActions(std::vector<FSEvent>& events);
  void sendFileAction(
    efsw::WatchID watchid,
    const std::string& dir,
    const std::string& filename,
    efsw::Action action,
    std::string oldFilename = ""
  );

  static void FSEventCallback(
    ConstFSEventStreamRef streamRef,
    void* userData,
    size_t numEvents,
    void* eventPaths,
    const FSEventStreamEventFlags eventFlags[],
    const FSEventStreamEventId eventIds[]
  );

  void handleAddModDel(
    efsw::WatchID handle,
    const uint32_t& flags,
    const std::string& path,
    std::string& dirPath,
		std::string& filePath
  );

  void process();

  bool isValid = true;

private:
  // RAII guard to ensure we un-mark our “processing” flag if we're destroyed
  // during processing.
  class ProcessingGuard {
    FSEventsFileWatcher& watcher;
  public:
    ProcessingGuard(FSEventsFileWatcher& w) : watcher(w) {}
    ~ProcessingGuard() {
      std::unique_lock<std::mutex> lock(watcher.processingMutex);
      watcher.isProcessing = false;
      watcher.processingComplete.notify_all();
    }
  };

  size_t removeHandle(efsw::WatchID handle);
  bool startNewStream();

  long nextHandleID;
  std::atomic<bool> isProcessing{false};
  std::atomic<bool> pendingDestruction{false};
  std::mutex processingMutex;
  std::mutex mapMutex;
  std::condition_variable processingComplete;

  // The running event stream that subscribes to all the paths we care about.
  FSEventStreamRef currentEventStream = nullptr;
  // An event stream that we create when our list of paths changes; it will
  // become the `currentEventStream` after it starts.
  FSEventStreamRef nextEventStream = nullptr;

  std::set<std::string> dirsChanged;

  std::unordered_map<efsw::WatchID, std::string> handlesToPaths;
  std::unordered_map<std::string, efsw::WatchID> pathsToHandles;
  std::unordered_map<efsw::WatchID, efsw::FileWatchListener*> handlesToListeners;
};
