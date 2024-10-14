#ifndef SRC_COMMON_H_
#define SRC_COMMON_H_

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include <vector>

#include "napi.h"
using namespace Napi;

#ifdef _WIN32
// Platform-dependent definition of HANDLE.
typedef HANDLE WatcherHandle;

// Conversion between V8 value and WatcherHandle.
Napi::Value WatcherHandleToV8Value(WatcherHandle handle, Napi::Env env);
WatcherHandle V8ValueToWatcherHandle(Napi::Value value);
bool IsV8ValueWatcherHandle(Napi::Value value);
#else
// Correspoding definitions on OS X and Linux.
typedef int32_t WatcherHandle;
#define WatcherHandleToV8Value(h, e) Napi::Number::New(e, h)
#define V8ValueToWatcherHandle(v) v.Int32Value()
#define IsV8ValueWatcherHandle(v) v.IsNumber()
#endif

void PlatformInit(Napi::Env env);
WatcherHandle PlatformWatch(const char* path, Napi::Env env);
void PlatformUnwatch(WatcherHandle handle, Napi::Env env);
bool PlatformIsHandleValid(WatcherHandle handle);
int PlatformInvalidHandleToErrorNumber(WatcherHandle handle);

enum EVENT_TYPE {
  EVENT_NONE,
  EVENT_CHANGE,
  EVENT_RENAME,
  EVENT_DELETE,
  EVENT_CHILD_CHANGE,
  EVENT_CHILD_RENAME,
  EVENT_CHILD_DELETE,
  EVENT_CHILD_CREATE,
};

struct PathWatcherEvent {
  EVENT_TYPE type;
  WatcherHandle handle;
  std::vector<char> new_path;
  std::vector<char> old_path;

  // Default constructor
  PathWatcherEvent() = default;

  // Constructor
  PathWatcherEvent(EVENT_TYPE t, WatcherHandle h, const std::vector<char>& np, const std::vector<char>& op = std::vector<char>())
    : type(t), handle(h), new_path(np), old_path(op) {}

  // Copy constructor
  PathWatcherEvent(const PathWatcherEvent& other)
    : type(other.type), handle(other.handle), new_path(other.new_path), old_path(other.old_path) {}

  // Copy assignment operator
  PathWatcherEvent& operator=(const PathWatcherEvent& other) {
    if (this != &other) {
      type = other.type;
      handle = other.handle;
      new_path = other.new_path;
      old_path = other.old_path;
    }
    return *this;
  }

  // Move constructor
  PathWatcherEvent(PathWatcherEvent&& other) noexcept
    : type(other.type), handle(other.handle),
    new_path(std::move(other.new_path)), old_path(std::move(other.old_path)) {}

  // Move assignment operator
  PathWatcherEvent& operator=(PathWatcherEvent&& other) noexcept {
    if (this != &other) {
      type = other.type;
      handle = other.handle;
      new_path = std::move(other.new_path);
      old_path = std::move(other.old_path);
    }
    return *this;
  }
};

using namespace Napi;

class PathWatcherWorker: public AsyncProgressQueueWorker<PathWatcherEvent> {
  public:
    PathWatcherWorker(Napi::Env env, Function &progressCallback);

    ~PathWatcherWorker() {}

    void Execute(const PathWatcherWorker::ExecutionProgress& progress) override;
    void OnOK() override;

    void OnProgress(const PathWatcherEvent* data, size_t) override;
    void Stop();

  private:
    Napi::Env _env;
    bool shouldStop = false;
    FunctionReference progressCallback;

    const char* GetEventTypeString(EVENT_TYPE type);
};

void PlatformThread(const PathWatcherWorker::ExecutionProgress& progress, bool& shouldStop, Napi::Env env);
void PlatformStop(Napi::Env env);

void CommonInit(Napi::Env env);

Napi::Value SetCallback(const Napi::CallbackInfo& info);
Napi::Value Watch(const Napi::CallbackInfo& info);
Napi::Value Unwatch(const Napi::CallbackInfo& info);

#endif  // SRC_COMMON_H_
