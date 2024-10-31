#include "core.h"
#include "include/efsw/efsw.hpp"
#include "napi.h"
#include <uv.h>
#include <string>

#ifdef DEBUG
#include <iostream>
#endif

#ifdef __APPLE__
#include <sys/stat.h>
#endif

#ifdef _WIN32
// Stub out this function on Windows. For now we don't have a need to compare
// watcher start times to file creation/modification times, so this isn't
// necessary. (Likewise, we're careful to conceal `PredatesWatchStart` calls
// behind `#ifdef`s, so we don’t need to define it at all.)
static int Now() { return 0; }
#else

// Returns the current Unix timestamp.
static timeval Now() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return tv;
}

// Given a Unix timestamp and a file `timespec`, decides whether the file’s
// timestamp predates the Unix timestamp. Used to compare creation/modification
// times to arbitrary points in time.
static bool PredatesWatchStart(struct timespec fileSpec, timeval startTime) {
  bool fileEventOlder = fileSpec.tv_sec < startTime.tv_sec || (
    (fileSpec.tv_sec == startTime.tv_sec) &&
    ((fileSpec.tv_nsec / 1000) < startTime.tv_usec)
  );
  return fileEventOlder;
}
#endif

static std::string EventType(efsw::Action action, bool isChild) {
  switch (action) {
    case efsw::Actions::Add:
      return isChild ? "child-create" : "create";
    case efsw::Actions::Delete:
      return isChild ? "child-delete" : "delete";
    case efsw::Actions::Modified:
      return isChild ? "child-change" : "change";
    case efsw::Actions::Moved:
      return isChild ? "child-rename" : "rename";
    default:
      return "unknown";
  }
}

// This is a bit hacky, but it allows us to stop invoking callbacks more
// quickly when the environment is terminating.
static bool EnvIsStopping(Napi::Env env) {
  PathWatcher* pw = env.GetInstanceData<PathWatcher>();
  return pw->isStopping;
}

// Ensure a given path has a trailing separator for comparison purposes.
static std::string NormalizePath(std::string path) {
  if (path.back() == PATH_SEPARATOR) return path;
  return path + PATH_SEPARATOR;
}

static void StripTrailingSlashFromPath(std::string& path) {
  if (path.empty() || (path.back() != '/')) return;
  path.pop_back();
}

static bool PathsAreEqual(std::string pathA, std::string pathB) {
  return NormalizePath(pathA) == NormalizePath(pathB);
}

// This is the main-thread function that receives all `ThreadSafeFunction`
// calls. It converts the `PathWatcherEvent` struct into JS values before
// invoking our callback.
static void ProcessEvent(
  Napi::Env env,
  Napi::Function callback,
  PathWatcherEvent* event
) {
  // Translate the event type to the expected event name in the JS code.
  //
  // NOTE: This library previously envisioned that some platforms would allow
  // watching of files directly and some would require watching of a file's
  // parent folder. EFSW uses the parent-folder approach on all platforms, so
  // in practice we're not using half of the event names we used to use. That's
  // why the second argument below is `true`.
  //
  // There might be some edge cases that we need to handle here; for instance,
  // if we're watching a directory and that directory itself is deleted, then
  // that should be `delete` rather than `child-delete`. Right now we deal with
  // that in JavaScript, but we could handle it here instead.
  if (EnvIsStopping(env)) return;

  std::string newPath;
  std::string oldPath;

  if (!event->new_path.empty()) {
    newPath.assign(event->new_path.begin(), event->new_path.end());
  }

  if (!event->old_path.empty()) {
    oldPath.assign(event->old_path.begin(), event->old_path.end());
  }

  // Since we watch directories, most sorts of events will only happen to files
  // within the directories…
  bool isChildEvent = true;
  if (PathsAreEqual(newPath, event->watcher_path)) {
    // …but the `delete` event can happen to the directory itself, in which
    // case we should report it as `delete` rather than `child-delete`.
    isChildEvent = false;
  }

  std::string eventName = EventType(event->type, isChildEvent);

  try {
    callback.Call({
      Napi::String::New(env, eventName),
      Napi::Number::New(env, event->handle),
      Napi::String::New(env, newPath),
      Napi::String::New(env, oldPath)
    });
  } catch (const Napi::Error& e) {
    // TODO: Unsure why this would happen.
    Napi::TypeError::New(env, "Unknown error handling filesystem event").ThrowAsJavaScriptException();
  }
}

PathWatcherListener::PathWatcherListener(
  Napi::Env env,
  Napi::ThreadSafeFunction tsfn
): tsfn(tsfn) {}

void PathWatcherListener::Stop() {
  if (isShuttingDown) return;
  // Prevent responders from acting while we shut down.
  std::lock_guard<std::mutex> lock(shutdownMutex);
  if (isShuttingDown) return;
  isShuttingDown = true;
}

void PathWatcherListener::Stop(FileWatcher* fileWatcher) {
  for (auto& it : paths) {
    fileWatcher->removeWatch(it.first);
  }
  paths.clear();
  Stop();
}

// Correlate a watch ID to a path/timestamp pair.
void PathWatcherListener::AddPath(PathTimestampPair pair, efsw::WatchID handle) {
  std::lock_guard<std::mutex> lock(pathsMutex);
  paths[handle] = pair;
  pathsToHandles[pair.path] = handle;
}

// Remove metadata for a given watch ID.
void PathWatcherListener::RemovePath(efsw::WatchID handle) {
  std::string path;
  {
    std::lock_guard<std::mutex> lock(pathsMutex);
    auto it = paths.find(handle);
#ifdef DEBUG
  std::cout << "Unwatching handle: [" << handle << "] path: [" << it->second.path << "]" << std::endl;
#endif

    if (it == paths.end()) return;
    path = it->second.path;
    paths.erase(it);
  }

  {
    std::lock_guard<std::mutex> lock(pathsToHandlesMutex);
    auto itp = pathsToHandles.find(path);
    if (itp == pathsToHandles.end()) return;
    pathsToHandles.erase(itp);
  }
}

bool PathWatcherListener::HasPath(std::string path) {
  std::lock_guard<std::mutex> lock(pathsToHandlesMutex);
  auto it = pathsToHandles.find(path);
  return it != pathsToHandles.end();
}

efsw::WatchID PathWatcherListener::GetHandleForPath(std::string path) {
  std::lock_guard<std::mutex> lock(pathsToHandlesMutex);
  auto it = pathsToHandles.find(path);
  return it->second;
}

bool PathWatcherListener::IsEmpty() {
  std::lock_guard<std::mutex> lock(pathsMutex);
  return paths.empty();
}

void PathWatcherListener::handleFileAction(
  efsw::WatchID watchId,
  const std::string& dir,
  const std::string& filename,
  efsw::Action action,
  std::string oldFilename
) {
#ifdef DEBUG
  std::cout << "PathWatcherListener::handleFileAction dir: " << dir << " filename: " << filename << " action: " << EventType(action, true) << std::endl;
#endif
  // Don't try to proceed if we've already started the shutdown process…
  if (isShuttingDown) return;

  // …but if we haven't, make sure that shutdown doesn’t happen until we’re
  // done.
  std::lock_guard<std::mutex> lock(shutdownMutex);
  if (isShuttingDown) return;

  // Extract the expected watcher path and (on macOS) the start time of the
  // watcher.
  PathTimestampPair pair;
  std::string realPath;
#ifdef __APPLE__
  timeval startTime;
#endif
  {
    std::lock_guard<std::mutex> lock(pathsMutex);
    auto it = paths.find(watchId);
    if (it == paths.end()) {
      // Couldn't find watcher. Assume it's been removed.
      return;
    }
    pair = it->second;
    realPath = pair.path;
#ifdef __APPLE__
    startTime = pair.timestamp;
#endif
  }

  std::string newPathStr = dir + filename;
  std::vector<char> newPath(newPathStr.begin(), newPathStr.end());

#ifdef __APPLE__
  // macOS seems to think that lots of file creations happen that aren't
  // actually creations; for instance, multiple successive writes to the same
  // file will sometimes nonsensically produce a `child-create` event preceding
  // each `child-change` event.
  //
  // Luckily, we can easily check whether or not a file has actually been
  // created on macOS: we can compare creation time to modification time. This
  // weeds out most of the false positives.
  {
    struct stat file;
    if (stat(newPathStr.c_str(), &file) != 0 && action != efsw::Action::Delete) {
      // If this was a delete action, the file is _expected_ not to exist
      // anymore. Otherwise it's a strange outcome and it means we should
      // ignore this event.
      return;
    }

    if (action == efsw::Action::Add) {
      // One easy way to check if a file was truly just created: does its
      // creation time match its modification time? If not, the file has been
      // written to since its creation.
      if (file.st_birthtimespec.tv_sec != file.st_mtimespec.tv_sec) {
        return;
      }

      // Next, weed out unnecessary `create` and `change` events that represent
      // file actions that happened before we started watching.
      if (PredatesWatchStart(file.st_birthtimespec, startTime)) {
#ifdef DEBUG
  std::cout << "File was created before we started this path watcher! (skipping)" << std::endl;
#endif
        return;
      }
    } else if (action == efsw::Action::Modified) {
      if (PredatesWatchStart(file.st_mtimespec, startTime)) {
#ifdef DEBUG
  std::cout << "File was modified before we started this path watcher! (skipping)" << std::endl;
#endif
        return;
      }
    }
  }
#endif

  std::vector<char> oldPath;
  if (!oldFilename.empty()) {
    std::string oldPathStr = dir + oldFilename;
    oldPath.assign(oldPathStr.begin(), oldPathStr.end());
  }

  if (!tsfn) return;
  napi_status status = tsfn.Acquire();
  if (status != napi_ok) {
    // We couldn't acquire the `tsfn`; it might be in the process of being
    // aborted because our environment is terminating.
    return;
  }

  // One (rare) special case we need to handle on all platforms:
  //
  // * Watcher exists on directory `/foo/bar`.
  // * Watcher exists on directory `/foo/bar/baz`.
  // * Directory `/foo/bar/baz` is deleted.
  //
  // In this instance, both watchers should be notified, but `efsw` will signal
  // only the `/foo/bar` watcher. (If only `/foo/bar/baz` were present, the
  // `/foo/bar/baz` watcher would be signalled instead.)
  //
  // Our custom macOS implementation replicates this incorrect behavior so that
  // we can handle this case uniformly in this one place.
  bool hasSecondMatch = false;
  efsw::WatchID secondHandle;

  // If we need to account for this scenario, then the full path will have its
  // own watcher. Since we only watch directories, this proves that the full
  // path is a directory.
  if (HasPath(newPathStr) && action == efsw::Action::Delete) {
    efsw::WatchID handle = GetHandleForPath(newPathStr);
    if (watchId != handle) {
      hasSecondMatch = true;
      secondHandle = handle;
    }
  }

  PathWatcherEvent* event = new PathWatcherEvent(action, watchId, newPath, oldPath, realPath);

  // TODO: Instead of calling `BlockingCall` once per event, throttle them by
  // some small amount of time (like 50-100ms). That will allow us to deliver
  // them in batches more efficiently — and for the wrapper JavaScript code to
  // do some elimination of redundant events.
  status = tsfn.BlockingCall(event, ProcessEvent);

  if (hasSecondMatch && status == napi_ok) {
    // In the rare case of the scenario described above, we have a second
    // callback invocation to make with a second event. Luckily, the only thing
    // that changes about the event is the handle!
    PathWatcherEvent* secondEvent = new PathWatcherEvent(action, secondHandle, newPath, oldPath, realPath);
    tsfn.BlockingCall(secondEvent, ProcessEvent);
  }

  tsfn.Release();
  if (status != napi_ok) {
    // TODO: Not sure how this could fail, or how we should present it to the
    // user if it does fail. This action runs on a separate thread and it's not
    // immediately clear how we'd surface an exception from here.
    delete event;
  }
}

static int next_env_id = 1;

PathWatcher::PathWatcher(Napi::Env env, Napi::Object exports) {
  envId = next_env_id++;

#ifdef DEBUG
  std::cout << "Initializing PathWatcher" << std::endl;
#endif

  DefineAddon(exports, {
    InstanceMethod("watch", &PathWatcher::Watch),
    InstanceMethod("unwatch", &PathWatcher::Unwatch),
    InstanceMethod("setCallback", &PathWatcher::SetCallback)
  });

  env.SetInstanceData<PathWatcher>(this);
}

PathWatcher::~PathWatcher() {
  isFinalizing = true;
  StopAllListeners();
}

// Watch a given path. Returns a handle.
Napi::Value PathWatcher::Watch(const Napi::CallbackInfo& info) {
  auto env = info.Env();
  // Record the current timestamp as early as possible. We'll use this as a way
  // of ignoring file-watcher events that happened before we started watching.
  auto now = Now();

  // First argument must be a string.
  if (!info[0].IsString()) {
    Napi::TypeError::New(env, "String required").ThrowAsJavaScriptException();
    return env.Null();
  }

  // Second argument is optional and tells us whether to use a recursive
  // watcher. Defaults to `false`.
  bool useRecursiveWatcher = false;
  if (info[1].IsBoolean()) {
    auto recursiveOption = info[1].As<Napi::Boolean>();
    useRecursiveWatcher = recursiveOption;
  }

  // The wrapper JS will resolve this to the file's real path. We expect to be
  // dealing with real locations on disk, since that's what EFSW will report to
  // us anyway.
  Napi::String path = info[0].ToString();
  std::string cppPath(path);

  StripTrailingSlashFromPath(cppPath);

#ifdef DEBUG
  std::cout << "PathWatcher::Watch path: [" << cppPath << "]" << std::endl;
#endif

  // It's invalid to call `watch` before having set a callback via
  // `setCallback`.
  if (callback.IsEmpty()) {
    Napi::TypeError::New(env, "No callback set").ThrowAsJavaScriptException();
    return env.Null();
  }

  if (!isWatching) {
#ifdef DEBUG
    std::cout << "  Creating ThreadSafeFunction and FileWatcher" << std::endl;
#endif
    tsfn = Napi::ThreadSafeFunction::New(
      env,
      callback.Value(),
      "pathwatcher-efsw-listener",
      0,
      1,
      [this](Napi::Env env) {
        // This is unexpected. We should try to do some cleanup before the
        // environment terminates.
        StopAllListeners();
      }
    );

    listener = new PathWatcherListener(env, tsfn);

#ifdef __APPLE__
  fileWatcher = new FSEventsFileWatcher();
#else
  fileWatcher = new efsw::FileWatcher();
  fileWatcher->followSymlinks(true);
  fileWatcher->watch();
#endif

    isWatching = true;
  }


  // EFSW represents watchers as unsigned `int`s; we can easily convert these
  // to JavaScript.
  WatcherHandle handle = fileWatcher->addWatch(cppPath, listener, useRecursiveWatcher);

#ifdef DEBUG
  std::cout << " handle: [" << handle << "]" << std::endl;
#endif

  if (handle >= 0) {
    // For each new watched path, remember both the normalized path and the
    // time we started watching it.
    PathTimestampPair pair = { cppPath, now };
    listener->AddPath(pair, handle);
  } else {
    auto error = Napi::Error::New(env, "Failed to add watch; unknown error");
    error.Set("code", Napi::Number::New(env, handle));
    error.ThrowAsJavaScriptException();
    return env.Null();
  }

  // The `watch` function returns a JavaScript number much like `setTimeout` or
  // `setInterval` would; this is the handle that the wrapper JavaScript can
  // use to unwatch the path later.
  return WatcherHandleToV8Value(handle, env);
}

// Unwatch the given handle.
Napi::Value PathWatcher::Unwatch(const Napi::CallbackInfo& info) {
  auto env = info.Env();
  if (!IsV8ValueWatcherHandle(info[0])) {
    Napi::TypeError::New(env, "Argument must be a number").ThrowAsJavaScriptException();
    return env.Null();
  }

  if (!listener) return env.Undefined();

  WatcherHandle handle = V8ValueToWatcherHandle(info[0].As<Napi::Number>());

  // EFSW doesn’t mind if we give it a handle that it doesn’t recognize; it’ll
  // just silently do nothing.
  //
  // This is useful because removing watcher can innocuously error anyway on
  // certain platforms. For instance, Linux will automatically stop watching a
  // directory when it gets deleted, and will then complain when you try to
  // stop the watcher that was already stopped. This shows up in debug logging
  // but is otherwise safe to ignore.
  fileWatcher->removeWatch(handle);
  listener->RemovePath(handle);

  if (listener->IsEmpty()) {
#ifdef DEBUG
    std::cout << "Cleaning up!" << std::endl;
#endif
    Cleanup(env);
    isWatching = false;
  }

  return env.Undefined();
}

void PathWatcher::StopAllListeners() {
  // This function is called internally in situations where we detect that the
  // environment is terminating. At that point, it's not safe to try to release
  // any `ThreadSafeFunction`s; but we can do the rest of the cleanup work
  // here.
  if (!isWatching) return;
  if (!listener) return;
  listener->Stop(fileWatcher);

  delete fileWatcher;
  isWatching = false;
}

// Set the JavaScript callback that will be invoked whenever a file changes.
//
// The user-facing API allows for an arbitrary number of different callbacks;
// this is an internal API for the wrapping JavaScript to use. That internal
// callback can multiplex to however many other callbacks need to be invoked.
void PathWatcher::SetCallback(const Napi::CallbackInfo& info) {
  auto env = info.Env();
  if (!info[0].IsFunction()) {
    Napi::TypeError::New(env, "Function required").ThrowAsJavaScriptException();
  }

  Napi::Function fn = info[0].As<Napi::Function>();
  callback.Reset();
  callback = Napi::Persistent(fn);
}

void PathWatcher::Cleanup(Napi::Env env) {
  StopAllListeners();

  if (!isFinalizing) {
    // `ThreadSafeFunction` wraps an internal `napi_threadsafe_function` that,
    // in some occasional scenarios, might already be `null` by the time we get
    // this far. It's not entirely understood why. But if that's true, we can
    // skip this part instead of trying to abort a function that doesn't exist
    // and causing a segfault.
    napi_threadsafe_function _tsfn = tsfn;
    if (_tsfn == nullptr) {
      return;
    }
    // The `ThreadSafeFunction` is the thing that will keep the environment
    // from terminating if we keep it open. When there are no active watchers,
    // we should release `tsfn`; when we add a new watcher thereafter, we can
    // create a new `tsfn`.
    tsfn.Abort();
  }
}

NODE_API_ADDON(PathWatcher)
