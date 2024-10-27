#pragma once

#include <napi.h>
#include <string>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include "../vendor/efsw/include/efsw/efsw.hpp"

#ifndef _WIN32
#include <sys/time.h>
#endif

#ifdef _WIN32
#define PATH_SEPARATOR '\\'
#else
#define PATH_SEPARATOR '/'
#endif

typedef efsw::WatchID WatcherHandle;

#ifdef _WIN32
struct PathTimestampPair {
  std::string path;
  int timestamp;
};
#else
struct PathTimestampPair {
  std::string path;
  timeval timestamp;
};
#endif

struct PathWatcherEvent {
  efsw::Action type;
  efsw::WatchID handle;
  std::vector<char> new_path;
  std::vector<char> old_path;

  std::string watcher_path;

  // Default constructor
  PathWatcherEvent() = default;

  // Constructor
  PathWatcherEvent(
    efsw::Action t,
    efsw::WatchID h,
    const std::vector<char>& np,
    const std::vector<char>& op = std::vector<char>(),
    const std::string& wp = ""
  ) : type(t), handle(h), new_path(np), old_path(op), watcher_path(wp) {}

  // Copy constructor
  PathWatcherEvent(const PathWatcherEvent& other)
    : type(other.type), handle(other.handle), new_path(other.new_path), old_path(other.old_path), watcher_path(other.watcher_path) {}

  // Copy assignment operator
  PathWatcherEvent& operator=(const PathWatcherEvent& other) {
    if (this != &other) {
      type = other.type;
      handle = other.handle;
      new_path = other.new_path;
      old_path = other.old_path;
      watcher_path = other.watcher_path;
    }
    return *this;
  }

  // Move constructor
  PathWatcherEvent(PathWatcherEvent&& other) noexcept
    : type(other.type), handle(other.handle),
    new_path(std::move(other.new_path)), old_path(std::move(other.old_path)), watcher_path(std::move(other.watcher_path)) {}

  // Move assignment operator
  PathWatcherEvent& operator=(PathWatcherEvent&& other) noexcept {
    if (this != &other) {
      type = other.type;
      handle = other.handle;
      new_path = std::move(other.new_path);
      old_path = std::move(other.old_path);
      watcher_path = std::move(other.watcher_path);
    }
    return *this;
  }
};

class PathWatcherListener: public efsw::FileWatchListener {
public:
  PathWatcherListener(
    Napi::Env env,
    Napi::ThreadSafeFunction tsfn
  );
  void handleFileAction(
    efsw::WatchID watchId,
    const std::string& dir,
    const std::string& filename,
    efsw::Action action,
    std::string oldFilename
  ) override;

  void AddPath(PathTimestampPair pair, efsw::WatchID handle);
  void RemovePath(efsw::WatchID handle);
  bool IsEmpty();
  void Stop();
  void Stop(efsw::FileWatcher* fileWatcher);

private:
  std::atomic<bool> isShuttingDown{false};
  std::mutex shutdownMutex;
  std::mutex pathsMutex;
  Napi::ThreadSafeFunction tsfn;
  std::unordered_map<efsw::WatchID, PathTimestampPair> paths;
};


#define WatcherHandleToV8Value(h, e) Napi::Number::New(e, h)
#define V8ValueToWatcherHandle(v) v.Int32Value()
#define IsV8ValueWatcherHandle(v) v.IsNumber()

class PathWatcher : public Napi::Addon<PathWatcher> {
public:
  PathWatcher(Napi::Env env, Napi::Object exports);
  ~PathWatcher();

  bool isStopping = false;

private:
  Napi::Value Watch(const Napi::CallbackInfo& info);
  Napi::Value Unwatch(const Napi::CallbackInfo& info);
  void SetCallback(const Napi::CallbackInfo& info);
  void Cleanup(Napi::Env env);
  void StopAllListeners();

  int envId;
  bool isFinalizing = false;
  bool isWatching = false;
  Napi::FunctionReference callback;
  Napi::ThreadSafeFunction tsfn;
  PathWatcherListener* listener;
  efsw::FileWatcher* fileWatcher = nullptr;
};
