#pragma once

#include <unordered_map>
#include <string>
#include <set>
#include <vector>
#include <mutex>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreServices/CoreServices.h>
#include "../../vendor/efsw/include/efsw/efsw.hpp"

template <typename K, typename V>
class BidirectionalMap {

public:
  void insert(const K& key, const V& value) {
    forward[key] = value;
    reverse[value] = key;
  }

  void remove(const K& key) {
    auto it = forward.find(key);
    if (it != forward.end()) {
      reverse.erase(it->second);
      forward.erase(it);
    }
  }

  const V* getValue(const K& key) const {
    auto it = forward.find(key);
    return it != forward.end() ? &it->second : nullptr;
  }

  const K* getKey(const V& value) const {
    auto it = reverse.find(value);
    return it != reverse.end() ? &it->second : nullptr;
  }

  std::unordered_map<K, V> forward;
  std::unordered_map<V, K> reverse;
};

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
  long nextHandleID;

  // The running event stream that subscribes to all the paths we care about.
  FSEventStreamRef currentEventStream = nullptr;
  // An event stream that we create when our list of paths changes; it will
  // become the `currentEventStream` after it starts.
  FSEventStreamRef nextEventStream = nullptr;

  std::set<std::string> dirsChanged;
  std::mutex dirsChangedMutex;

  BidirectionalMap<efsw::WatchID, std::string> handlesToPaths;
  std::unordered_map<efsw::WatchID, efsw::FileWatchListener*> handlesToListeners;
};
