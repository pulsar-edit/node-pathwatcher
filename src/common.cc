#include "common.h"
#include "addon-data.h"
#include "uv.h"

#include <iostream>

using namespace Napi;

void CommonInit(Napi::Env env) {
  auto addonData = env.GetInstanceData<AddonData>();
  addonData->watch_count = 0;
}

PathWatcherWorker::PathWatcherWorker(Napi::Env env, Function &progressCallback) :
  AsyncProgressQueueWorker(env), _env(env) {
  shouldStop = false;
  this->progressCallback.Reset(progressCallback);
}

void PathWatcherWorker::Execute(
  const PathWatcherWorker::ExecutionProgress& progress
) {
  PlatformThread(progress, shouldStop, _env);
}

void PathWatcherWorker::Stop() {
  Napi::Env env = Env();
  auto addonData = env.GetInstanceData<AddonData>();
  std::cout << "PathWatcherWorker::Stop for ID: " << addonData->id << std::endl;

  shouldStop = true;
}

const char* PathWatcherWorker::GetEventTypeString(EVENT_TYPE type) {
  switch (type) {
    case EVENT_CHANGE: return "change";
    case EVENT_DELETE: return "delete";
    case EVENT_RENAME: return "rename";
    case EVENT_CHILD_CREATE: return "child-create";
    case EVENT_CHILD_CHANGE: return "child-change";
    case EVENT_CHILD_DELETE: return "child-delete";
    case EVENT_CHILD_RENAME: return "child-rename";
    default: return "unknown";
  }
}

void PathWatcherWorker::OnProgress(const PathWatcherEvent* data, size_t) {
  Napi::Env env = Env();
  HandleScope scope(env);
  auto addonData = env.GetInstanceData<AddonData>();
  std::cout << "OnProgress reporting event for environment with ID: " << addonData->id << std::endl;
  if (this->progressCallback.IsEmpty()) return;
  std::string newPath(data->new_path.begin(), data->new_path.end());
  std::string oldPath(data->old_path.begin(), data->old_path.end());

  this->progressCallback.Call({
    Napi::String::New(Env(), GetEventTypeString(data->type)),
    WatcherHandleToV8Value(data->handle, Env()),
    Napi::String::New(Env(), newPath),
    Napi::String::New(Env(), oldPath)
  });
}

void PathWatcherWorker::OnOK() {}

// Called when the first watcher is created.
void Start(Napi::Env env) {
  // std::cout << "Start" << std::endl;
  Napi::HandleScope scope(env);
  auto addonData = env.GetInstanceData<AddonData>();
  if (!addonData->callback) {
    return;
  }

  Napi::Function fn = addonData->callback.Value();

  addonData->worker = new PathWatcherWorker(env, fn);
  addonData->worker->Queue();
}

// Called when the last watcher is stopped.
void Stop(Napi::Env env) {
  auto addonData = env.GetInstanceData<AddonData>();
  std::cout << "Stop for ID: " << addonData->id << std::endl;
  PlatformStop(env);
  std::cout << "PlatformStop exited for ID: " << addonData->id << std::endl;
  if (addonData->worker) {
    addonData->worker->Stop();
  }
}

Napi::Value SetCallback(const Napi::CallbackInfo& info) {
  // std::cout << "SetCallback" << std::endl;
  auto env = info.Env();
  Napi::HandleScope scope(env);

  if (!info[0].IsFunction()) {
    Napi::TypeError::New(env, "Function required").ThrowAsJavaScriptException();
    return env.Null();
  }

  auto addonData = env.GetInstanceData<AddonData>();
  if (addonData->worker) {
    // std::cout << "Worker already exists" << std::endl;
  }
  addonData->callback.Reset(info[0].As<Napi::Function>(), 1);

  return env.Undefined();
}

Napi::Value Watch(const Napi::CallbackInfo& info) {
  // std::cout << "Watch" << std::endl;
  auto env = info.Env();
  auto addonData = env.GetInstanceData<AddonData>();
  Napi::HandleScope scope(env);

  if (!info[0].IsString()) {
    Napi::TypeError::New(env, "String required").ThrowAsJavaScriptException();
    return env.Null();
  }

  Napi::String path = info[0].ToString();
  std::string cppPath(path);
  WatcherHandle handle = PlatformWatch(cppPath.c_str(), env);

  if (!PlatformIsHandleValid(handle)) {
    int error_number = PlatformInvalidHandleToErrorNumber(handle);
    Napi::Error err = Napi::Error::New(env, "Unable to watch path");

    if (error_number != 0) {
      err.Set("errno", Napi::Number::New(env, error_number));
      err.Set(
        "code",
        Napi::String::New(env, uv_err_name(-error_number))
      );
    }
    err.ThrowAsJavaScriptException();
    return env.Undefined();
  }

  if (addonData->watch_count++ == 0)
    Start(env);

  return WatcherHandleToV8Value(handle, info.Env());
}

Napi::Value Unwatch(const Napi::CallbackInfo& info) {
  // std::cout << "Unwatch" << std::endl;
  auto env = info.Env();
  auto addonData = env.GetInstanceData<AddonData>();
  Napi::HandleScope scope(env);

  if (!IsV8ValueWatcherHandle(info[0])) {
    Napi::TypeError::New(
      env,
      "Local type required"
    ).ThrowAsJavaScriptException();
    return env.Null();
  }

#ifdef _WIN32
  Napi::Value num = info[0];
#else
  Napi::Number num = info[0].ToNumber();
#endif

  PlatformUnwatch(V8ValueToWatcherHandle(num), env);

  if (--addonData->watch_count == 0)
    Stop(env);

  return env.Undefined();
}
