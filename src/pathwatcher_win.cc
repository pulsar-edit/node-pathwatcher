#include <atomic>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <iostream>
#include <queue>
#include <thread>
#include <unordered_map>
#include "common.h"
#include "addon-data.h"
#include "js_native_api_types.h"
#include "napi.h"
#include "uv.h"

struct ThreadData {
  std::queue<PathWatcherEvent> event_queue;
  std::mutex mutex;
  std::condition_variable cv;
  const PathWatcherWorker::ExecutionProgress* progress;
  bool should_stop = false;
  bool is_main = false;
  bool is_working = false;
};

class ThreadManager {
public:
  void register_thread(
    int id,
    const PathWatcherWorker::ExecutionProgress* progress,
    bool is_main
  ) {
    std::lock_guard<std::mutex> lock(mutex_);
    threads_[id] = std::make_unique<ThreadData>();
    threads_[id]->progress = progress;
    threads_[id]->is_main = is_main;
    if (is_main) {
      this->main_id = id;
    }
  }

  bool clock_out(int id) {
    std::cout << "omg clocking out sanity check " << id << std::endl;
    if (id != this->main_id) {
      std::cout << "Doesn't think it's the boss thread! " << id << std::endl;
      return false;
    }
    // std::lock_guard<std::mutex> lock(mutex_);
    auto current_boss_data = this->get_thread_data(this->main_id);
    // Pretend this is still the main thread until we can hand off duties to
    // another thread.
    current_boss_data->is_main = true;
    current_boss_data->is_working = false;

    std::cout << "Finding a new boss thread." << std::endl;

    for (const auto& pair : threads_) {
      if (pair.first != id) {
        std::cout << "Promoting " << pair.first << " to be the new boss thread." << std::endl;
        promote(pair.first);
        if (this->main_id != pair.first) {
          std::cout << "WHY AM I GOING INSANE?" << std::endl;
        }
        auto new_boss_data = this->get_thread_data(pair.first);
        new_boss_data->is_main = true;
        return true;
      }
    }
    return false;
  }

  bool clock_in(int id) {
    std::cout << "@@@@ clock_in called: Thread " << id << std::endl;
    std::lock_guard<std::mutex> lock(mutex_);
    if (id != this->main_id) return false;
    std::cout << "@@@@ CLOCKING IN: Thread " << id << std::endl;

    auto boss_data = unsafe_get_thread_data(id);
    boss_data->is_working = true;
    boss_data->cv.notify_one();
    return true;
  }

  bool unregister_thread(int id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return threads_.erase(id) > 0;
  }

  int has_main () {
    return this->main_id > -1;
  }

  bool is_main (int id) {
    return id == this->main_id;
  }

  void wake_up_new_main() {
    if (this->main_id == -1) return;
    std::cout << "Attempting to wake up new main thread: " << this->main_id << std::endl;
    auto new_boss_data = this->get_thread_data(this->main_id);
    new_boss_data->cv.notify_one();
  }

  void promote(int id) {
    auto data = this->get_thread_data(id);
    data->is_main = true;
    this->main_id = id;
  }

  void queue_event(int id, PathWatcherEvent event) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (threads_.count(id) > 0) {
      std::lock_guard<std::mutex> thread_lock(threads_[id]->mutex);
      threads_[id]->event_queue.push(std::move(event));
      threads_[id]->cv.notify_one();
    }
  }

  void stop_all() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& pair : threads_) {
      std::lock_guard<std::mutex> thread_lock(pair.second->mutex);
      pair.second->should_stop = true;
      pair.second->cv.notify_one();
    }
  }

  bool is_empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return threads_.empty();
  }

  int size() {
    std::lock_guard<std::mutex> lock(mutex_);
    return threads_.size();
  }

  bool has_thread(int id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return threads_.find(id) != threads_.end();
  }

  ThreadData* get_thread_data(int id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = threads_.find(id);
    return it != threads_.end() ? it->second.get() : nullptr;
  }

  ThreadData* get_main_data() {
    if (this->main_id == -1) return nullptr;
    return this->get_thread_data(this->main_id);
  }

  std::unordered_map<int, std::unique_ptr<ThreadData>> threads_;

private:

  ThreadData* unsafe_get_thread_data(int id) {
    auto it = threads_.find(id);
    return it != threads_.end() ? it->second.get() : nullptr;
  }


  mutable std::mutex mutex_;
  int main_id = -1;
};

// Global instance
ThreadManager g_thread_manager;

// Global atomic flag to ensure only one PlatformThread is running
std::atomic<bool> g_platform_thread_running(false);

void ThreadWorker(int id) {
  while (true) {
    ThreadData* thread_data = g_thread_manager.get_thread_data(id);
    if (!thread_data) break; // (thread was unregistered)

    std::unique_lock<std::mutex> lock(thread_data->mutex);
    std::cout << "[WAIT WAIT WAIT] ThreadWorker with ID: " << id << " has should_stop of: " << thread_data->should_stop << std::endl;
    thread_data->cv.wait(lock, [thread_data] {
      if (thread_data->should_stop) return true;
      if (!thread_data->event_queue.empty()) return true;
      return false;
    });

    // std::cout << "ThreadWorker with ID: " << id << "is unblocked. Why? " << "(internal_stop? " << thread_data->internal_stop << ") (should_stop? " << *(thread_data->should_stop) << ") (items in queue? " << !thread_data->event_queue.empty() << ")" << std::endl;

    if (thread_data->should_stop && thread_data->event_queue.empty()) {
      break;
    }

    while (!thread_data->event_queue.empty()) {
      auto event = thread_data->event_queue.front();
      thread_data->event_queue.pop();
      lock.unlock();
      std::cout << "ThreadWorker with ID: " << id << " is sending event!" << std::endl;
      thread_data->progress->Send(&event, 1);
      lock.lock();

      if (thread_data->should_stop) break;
    }

    if (thread_data->should_stop) break;
  }
}

// Global instance of VectorMap
// ProgressMap g_progress_map;

// Size of the buffer to store result of ReadDirectoryChangesW.
static const unsigned int kDirectoryWatcherBufferSize = 4096;

// Mutex for the HandleWrapper map.
static uv_mutex_t g_handle_wrap_map_mutex;

// The events to be waited on.
static std::vector<HANDLE> g_events;

// The dummy event to wakeup the thread.
static HANDLE g_wake_up_event;

// The dummy event to ensure we are not waiting on a file handle when destroying it.
static HANDLE g_file_handles_free_event;

// static bool g_is_running = false;
// static int g_env_count = 0;

struct ScopedLocker {
  explicit ScopedLocker(uv_mutex_t& mutex) : mutex_(&mutex) { uv_mutex_lock(mutex_); }
  ~ScopedLocker() { Unlock(); }

  void Unlock() { uv_mutex_unlock(mutex_); }

  uv_mutex_t* mutex_;
};

struct HandleWrapper {
  HandleWrapper(WatcherHandle handle, const char* path_str, int addon_data_id)
      : addonDataId(addon_data_id),
        dir_handle(handle),
        path(strlen(path_str)),
        canceled(false) {
    memset(&overlapped, 0, sizeof(overlapped));
    overlapped.hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    g_events.push_back(overlapped.hEvent);

    std::copy(path_str, path_str + path.size(), path.data());
    map_[overlapped.hEvent] = this;
  }

  ~HandleWrapper() {
    if (!canceled) {
      Cancel();
    }

    CloseHandle(dir_handle);
    CloseHandle(overlapped.hEvent);
  }

  void Cancel() {
    canceled = true;
    CancelIoEx(dir_handle, &overlapped);
    g_events.erase(std::remove(g_events.begin(), g_events.end(), overlapped.hEvent), g_events.end());
    map_.erase(overlapped.hEvent);
  }

  int addonDataId;
  WatcherHandle dir_handle;
  std::vector<char> path;
  bool canceled;
  OVERLAPPED overlapped;
  char buffer[kDirectoryWatcherBufferSize];

  static HandleWrapper* Get(HANDLE key) { return map_[key]; }

  static std::map<WatcherHandle, HandleWrapper*> map_;
};

std::map<WatcherHandle, HandleWrapper*> HandleWrapper::map_;

struct WatcherEvent {
  EVENT_TYPE type;
  WatcherHandle handle;
  std::vector<char> new_path;
  std::vector<char> old_path;
};

static bool QueueReaddirchanges(HandleWrapper* handle) {
  return ReadDirectoryChangesW(
    handle->dir_handle,
    handle->buffer,
    kDirectoryWatcherBufferSize,
    FALSE,
    FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_ATTRIBUTES |
     FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_LAST_ACCESS |
     FILE_NOTIFY_CHANGE_CREATION | FILE_NOTIFY_CHANGE_SECURITY,
    NULL,
    &handle->overlapped,
    NULL
  ) == TRUE;
}

Napi::Value WatcherHandleToV8Value(WatcherHandle handle, Napi::Env env) {
  uint64_t handleInt = reinterpret_cast<uint64_t>(handle);
  return Napi::BigInt::New(env, handleInt);
}

WatcherHandle V8ValueToWatcherHandle(Napi::Value value) {
  if (!value.IsBigInt()) {
    return NULL;
  }
  bool lossless;
  uint64_t handleInt = value.As<Napi::BigInt>().Uint64Value(&lossless);
  if (!lossless) {
    return NULL;
  }
  return reinterpret_cast<HANDLE>(handleInt);
}

bool IsV8ValueWatcherHandle(Napi::Value value) {
  return value.IsBigInt();
}

void PlatformInit(Napi::Env _env) {
  uv_mutex_init(&g_handle_wrap_map_mutex);

  g_file_handles_free_event = CreateEvent(NULL, TRUE, TRUE, NULL);
  g_wake_up_event = CreateEvent(NULL, FALSE, FALSE, NULL);
  g_events.push_back(g_wake_up_event);
}

void PlatformThread(
  const PathWatcherWorker::ExecutionProgress& progress,
  bool& shouldStop,
  Napi::Env env
) {
  auto addonData = env.GetInstanceData<AddonData>();

  bool hasMainThread = !g_thread_manager.is_empty();
  // bool expected = false;
  // bool hasMainThread = g_platform_thread_running.compare_exchange_strong(expected, true);

  if (!g_thread_manager.has_thread(addonData->id)) {
    g_thread_manager.register_thread(addonData->id, &progress, !hasMainThread);
  }

  ThreadData* thread_data = g_thread_manager.get_thread_data(addonData->id);

  std::cout << "Is " << addonData->id << " the boss thread? " << g_thread_manager.is_main(addonData->id) << std::endl;

  if (!g_thread_manager.is_main(addonData->id)) {
    while (!g_thread_manager.is_main(addonData->id)) {
      std::cout << "[WAIT WAIT " << addonData->id << "] Thread with ID: " << addonData->id << " in holding pattern" << std::endl;
      // A holding-pattern loop for threads that aren't the “boss” thread.
      ThreadData* thread_data = g_thread_manager.get_thread_data(addonData->id);
      if (!thread_data) break; // (thread was unregistered)
      if (g_thread_manager.is_main(addonData->id)) break;

      std::unique_lock<std::mutex> lock(thread_data->mutex);
      thread_data->cv.wait(lock, [thread_data] {
        if (thread_data->should_stop) return true;
        if (!thread_data->event_queue.empty()) return true;
        if (thread_data->is_main) return true;
        return false;
      });

      std::cout << "PEON Thread with ID: " << addonData->id << " unblocked because: (should_stop? " << thread_data->should_stop << ") (is_main? " << thread_data->is_main << ") (event queue? " << !thread_data->event_queue.empty() << ")" << std::endl;

      if (thread_data->should_stop && thread_data->event_queue.empty()) {
        break;
      }

      while (!thread_data->event_queue.empty()) {
        auto event = thread_data->event_queue.front();
        thread_data->event_queue.pop();
        lock.unlock();
        thread_data->progress->Send(&event, 1);
        lock.lock();

        if (thread_data->should_stop) break;
      }

      if (thread_data->is_main) {
        std::cout << "THIS IS WHERE WE WOULD HAVE THE NEW GUY CLOCK IN! " << addonData->id << std::endl;
        g_thread_manager.clock_in(addonData->id);
        break;
      }

      if (thread_data->should_stop) break;
    }
  }

  // if (!g_thread_manager.is_main(addonData->id)) {
  //   // If we get to this point and this still isn't the “boss” thread, then
  //   // we’ve broken out of the above loop but should not proceed. This thread
  //   // hasn't been promoted; it should stop.
  //   std::cout << "Thread with ID: " << addonData->id << " is not the new boss thread, so it must be exiting."
  //   g_thread_manager.unregister_thread(addonData->id);
  //   return;
  // }

  // If we get this far, then this is the main thread — either because it was
  // the first to be created or because it's been promoted after another thread
  // was stopped.

  std::cout << "PlatformThread ID: " << addonData->id << std::endl;

  // std::cout << "PlatformThread" << std::endl;
  if (g_thread_manager.is_main(addonData->id)) {
    while (!thread_data->should_stop && !shouldStop) {
      // Do not use g_events directly, since reallocation could happen when there
      // are new watchers adding to g_events when WaitForMultipleObjects is still
      // polling.
      ScopedLocker locker(g_handle_wrap_map_mutex);
      std::vector<HANDLE> copied_events(g_events);
      locker.Unlock();

      ResetEvent(g_file_handles_free_event);
      std::cout << "Thread with ID: " << addonData->id << " is waiting..." << std::endl;
      DWORD r = WaitForMultipleObjects(
        copied_events.size(),
        copied_events.data(),
        FALSE,
        100
      );
      SetEvent(g_file_handles_free_event);

      std::cout << "BOSS Thread with ID: " << addonData->id << " unblocked because: (should_stop? " << thread_data->should_stop << ") (shouldStop? " << shouldStop << ") (is_main? " << thread_data->is_main << ") (event queue? " << (copied_events.size() > 1) << ") [THREAD SIZE: " << g_thread_manager.size() << "]" << std::endl;

      if (!thread_data->is_main) {
        break;
      }

      if (r == WAIT_TIMEOUT) {
        continue;
      }
      std::cout << "Thread with ID: " << addonData->id << " is done waiting." << std::endl;

      int i = r - WAIT_OBJECT_0;
      if (i >= 0 && i < copied_events.size()) {
        // It's a wake up event; there is no FS event.
        if (copied_events[i] == g_wake_up_event) {
          std::cout << "Thread with ID: " << addonData->id << " received wake-up event. Continuing." << std::endl;
          if (!thread_data->is_main) break;
          continue;
        }

        ScopedLocker locker(g_handle_wrap_map_mutex);

        // Match up the filesystem event with the handle responsible for it.
        HandleWrapper* handle = HandleWrapper::Get(copied_events[i]);
        if (!handle || handle->canceled) {
          continue;
        }

        if (!g_thread_manager.has_thread(handle->addonDataId)) {
          // Ignore handles that belong to stale environments.
          std::cout << "Unrecognized environment: " << handle->addonDataId << std::endl;
          continue;
        }

        DWORD bytes_transferred;
        if (!GetOverlappedResult(handle->dir_handle, &handle->overlapped, &bytes_transferred, FALSE)) {
          std::cout << "Nothing for thread: " << addonData->id << std::endl;
          continue;
        }
        if (bytes_transferred == 0) {
          std::cout << "Nothing for thread: " << addonData->id << std::endl;
          continue;
        }

        std::vector<char> old_path;
        std::vector<WatcherEvent> events;

        DWORD offset = 0;
        while (true) {
          FILE_NOTIFY_INFORMATION* file_info =
              reinterpret_cast<FILE_NOTIFY_INFORMATION*>(handle->buffer + offset);

          // Emit events for children.
          EVENT_TYPE event = EVENT_NONE;
          switch (file_info->Action) {
            case FILE_ACTION_ADDED:
              event = EVENT_CHILD_CREATE;
              break;
            case FILE_ACTION_REMOVED:
              event = EVENT_CHILD_DELETE;
              break;
            case FILE_ACTION_RENAMED_OLD_NAME:
              event = EVENT_CHILD_RENAME;
              break;
            case FILE_ACTION_RENAMED_NEW_NAME:
              event = EVENT_CHILD_RENAME;
              break;
            case FILE_ACTION_MODIFIED:
              event = EVENT_CHILD_CHANGE;
              break;
          }

          if (event != EVENT_NONE) {
            // The FileNameLength is in "bytes", but the WideCharToMultiByte
            // requires the length to be in "characters"!
            int file_name_length_in_characters =
                file_info->FileNameLength / sizeof(wchar_t);

            char filename[MAX_PATH] = { 0 };
            int size = WideCharToMultiByte(
              CP_UTF8,
              0,
              file_info->FileName,
              file_name_length_in_characters,
              filename,
              MAX_PATH,
              NULL,
              NULL
            );

            // Convert file name to file path, same with:
            // path = handle->path + '\\' + filename
            std::vector<char> path(handle->path.size() + 1 + size);
            std::vector<char>::iterator iter = path.begin();
            iter = std::copy(handle->path.begin(), handle->path.end(), iter);
            *(iter++) = '\\';
            std::copy(filename, filename + size, iter);

            if (file_info->Action == FILE_ACTION_RENAMED_OLD_NAME) {
              // Do not send rename event until the NEW_NAME event, but still keep
              // a record of old name.
              old_path.swap(path);
            } else if (file_info->Action == FILE_ACTION_RENAMED_NEW_NAME) {
              WatcherEvent e = { event, handle->overlapped.hEvent };
              e.new_path.swap(path);
              e.old_path.swap(old_path);
              events.push_back(e);
            } else {
              WatcherEvent e = { event, handle->overlapped.hEvent };
              e.new_path.swap(path);
              events.push_back(e);
            }
          }

          if (file_info->NextEntryOffset == 0) break;
          offset += file_info->NextEntryOffset;
        }

        // Restart the monitor, it was reset after each call.
        QueueReaddirchanges(handle);

        locker.Unlock();

        std::cout << "Total events processed on thread " << addonData->id << ": " << events.size() << std::endl;

        for (size_t i = 0; i < events.size(); ++i) {
          std::cout << "Emitting " << events[i].type << " event on thread " << addonData->id << " for path: " << events[i].new_path.data() << " for worker with ID: " << handle->addonDataId << std::endl;
          PathWatcherEvent event(
            events[i].type,
            events[i].handle,
            events[i].new_path,
            events[i].old_path
          );
          if (handle->addonDataId == addonData->id) {
            // This event belongs to our thread, so we can handle it directly.
            std::cout << "Invoking directly " << addonData->id << std::endl;
            progress.Send(&event, 1);
          } else {
            // Since it's not ours, we should enqueue it to be handled by the
            // thread responsible for it.
            g_thread_manager.queue_event(handle->addonDataId, event);
          }
        }
      }
    } // while
  }


  std::cout << "ABOUT TO UNREGISTER!!!!!!" << addonData->id << std::endl;
  g_thread_manager.unregister_thread(addonData->id);
  std::cout << "PlatformThread with ID: " << addonData->id << " is exiting! Thread size: " << g_thread_manager.size() << std::endl;

  if (!g_thread_manager.is_empty()) {
    std::cout << "[???] Waking up new main!" << std::endl;
    // g_thread_manager.wake_up_new_main();
    // Sleep briefly to allow time for another thread to wake up.
    std::cout << "[???] Sleeping!" << std::endl;

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    std::cout << "[???] Done sleeping!" << std::endl;
  }

  // g_platform_thread_running = false;
}

// // Function to get the vector for a given AddonData
// std::shared_ptr<VectorMap::Vector> GetVectorForAddonData(AddonData* addonData) {
//   return g_vector_map.get_or_create(addonData->id);
// }

WatcherHandle PlatformWatch(const char* path, Napi::Env env) {
  auto addonData = env.GetInstanceData<AddonData>();
  std::cout << "PlatformWatch ID: " << addonData->id << " Path: " << path << std::endl;
  wchar_t wpath[MAX_PATH] = { 0 };
  MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, MAX_PATH);

  // Requires a directory, file watching is emulated in js.
  DWORD attr = GetFileAttributesW(wpath);
  if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
    return INVALID_HANDLE_VALUE;
  }

  WatcherHandle dir_handle = CreateFileW(
    wpath,
    FILE_LIST_DIRECTORY,
    FILE_SHARE_READ | FILE_SHARE_DELETE | FILE_SHARE_WRITE,
    NULL,
    OPEN_EXISTING,
    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
    NULL
  );

  if (!PlatformIsHandleValid(dir_handle)) {
    return INVALID_HANDLE_VALUE;
  }

  std::unique_ptr<HandleWrapper> handle;
  {
    ScopedLocker locker(g_handle_wrap_map_mutex);
    handle.reset(new HandleWrapper(dir_handle, path, addonData->id));
  }

  if (!QueueReaddirchanges(handle.get())) {
    return INVALID_HANDLE_VALUE;
  }

  // Wake up the thread to add the new event.
  SetEvent(g_wake_up_event);

  // The pointer is leaked if no error happened.
  return handle.release()->overlapped.hEvent;
}

void PlatformUnwatch(WatcherHandle key, Napi::Env env) {
  auto addonData = env.GetInstanceData<AddonData>();
  std::cout << "PlatformUnwatch ID: " << addonData->id << std::endl;
  if (PlatformIsHandleValid(key)) {
    HandleWrapper* handle;
    {
      ScopedLocker locker(g_handle_wrap_map_mutex);
      handle = HandleWrapper::Get(key);
      handle->Cancel();
    }

    do {
      SetEvent(g_wake_up_event);
    } while (WaitForSingleObject(g_file_handles_free_event, 50) == WAIT_TIMEOUT);
    delete handle;
  }
}

bool PlatformIsHandleValid(WatcherHandle handle) {
  return handle != INVALID_HANDLE_VALUE;
}

// We have no errno on Windows.
int PlatformInvalidHandleToErrorNumber(WatcherHandle handle) {
  return 0;
}

void PlatformStop(Napi::Env env) {
  auto addonData = env.GetInstanceData<AddonData>();
  std::cout << "@@@@ PlatformStop ID: " << addonData->id << std::endl;
  auto thread_data = g_thread_manager.get_thread_data(addonData->id);

  // if (g_thread_manager.is_main(addonData->id)) {
  //   std::cout << "CLOCKING OUT!!!!!!" << addonData->id << std::endl;
  //   // Warm hand-off.
  //   g_thread_manager.clock_out(addonData->id);
  //
  //   if (g_thread_manager.size() > 1) {
  //     auto new_main_thread_data = g_thread_manager.get_main_data();
  //     g_thread_manager.wake_up_new_main();
  //     std::unique_lock<std::mutex> lock(new_main_thread_data->mutex);
  //     // Wait until the new boss clocks in.
  //     new_main_thread_data->cv.wait(lock, [new_main_thread_data] {
  //       return new_main_thread_data->is_working;
  //     });
  //   }
  // }

  if (thread_data) {
    if (thread_data->is_main) {
      std::cout << "CLOCKING OUT!!!!!!" << addonData->id << " thread size: " << g_thread_manager.size() << std::endl;
      g_thread_manager.clock_out(addonData->id);

      if (g_thread_manager.size() > 1) {
        std::cout << "WAITING FOR NEW BOSS!!!!!!" << addonData->id << std::endl;
        auto new_main_thread_data = g_thread_manager.get_main_data();
        g_thread_manager.wake_up_new_main();
        std::unique_lock<std::mutex> lock(new_main_thread_data->mutex);
        // Wait until the new boss clocks in.
        new_main_thread_data->cv.wait(lock, [new_main_thread_data] {
          return new_main_thread_data->is_working;
        });
        std::cout << "PROCEEDING!!!!!!" << addonData->id << std::endl;
      } else {
        std::cout << "WAS LAST THREAD!!!!!!" << addonData->id << std::endl;
      }
    } else {
      std::cout << "NO THREAD DATA!!!!!!" << addonData->id << std::endl;
    }


    std::lock_guard<std::mutex> lock(thread_data->mutex);
    thread_data->should_stop = true;
    thread_data->cv.notify_one();
    // g_thread_manager.unregister_thread(addonData->id);
  }
}
