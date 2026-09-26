#include "ReadDirectoryChangesFileWatcher.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstddef>
#include <vector>

#ifdef DEBUG
#include <iostream>
#endif

namespace {

// Children appearing, disappearing, being renamed, or having their contents
// changed. We don't ask for attribute, security, or last-access changes.
constexpr DWORD kNotifyFilter =
    FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
    FILE_NOTIFY_CHANGE_CREATION | FILE_NOTIFY_CHANGE_LAST_WRITE |
    FILE_NOTIFY_CHANGE_SIZE;

// Must stay under 64KB: `ReadDirectoryChangesW` fails with
// `ERROR_INVALID_PARAMETER` for larger buffers when the directory is on a
// network share.
constexpr DWORD kBufferBytes = 63 * 1024;

std::wstring Utf8ToWide(const std::string &str) {
  if (str.empty())
    return std::wstring();
  int len = MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(),
                                nullptr, 0);
  if (len <= 0)
    return std::wstring();
  std::wstring result(len, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), &result[0],
                      len);
  return result;
}

std::string WideToUtf8(const wchar_t *str, size_t length) {
  if (length == 0)
    return std::string();
  int len = WideCharToMultiByte(CP_UTF8, 0, str, (int)length, nullptr, 0,
                                nullptr, nullptr);
  if (len <= 0)
    return std::string();
  std::string result(len, '\0');
  WideCharToMultiByte(CP_UTF8, 0, str, (int)length, &result[0], len, nullptr,
                      nullptr);
  return result;
}

// Converts a UTF-8 path to a wide path that Win32 file APIs will accept at any
// length. Past a certain length they need the `\\?\` prefix, which in turn
// means the path must be absolute and use only backslashes.
std::wstring ToWidePath(const std::string &path) {
  std::wstring wide = Utf8ToWide(path);
  // `CreateFileW` on a directory tops out 12 characters short of `MAX_PATH`,
  // leaving room for an 8.3 filename.
  if (wide.size() < MAX_PATH - 12 || wide.rfind(L"\\\\?\\", 0) == 0)
    return wide;

  for (auto &ch : wide) {
    if (ch == L'/')
      ch = L'\\';
  }

  if (wide.size() >= 3 && wide[1] == L':' && wide[2] == L'\\')
    return L"\\\\?\\" + wide;
  if (wide.rfind(L"\\\\", 0) == 0)
    return L"\\\\?\\UNC\\" + wide.substr(2);
  return wide;
}

// Returns the path that `handle` currently resolves to, or an empty string if
// it can't be determined — which includes the case where the directory has
// been deleted.
std::wstring GetHandlePath(HANDLE handle) {
  if (handle == nullptr || handle == INVALID_HANDLE_VALUE)
    return std::wstring();

  // Called with a null buffer, this returns the length required *including*
  // the null terminator; called with a buffer, it returns the number of
  // characters written *excluding* it.
  DWORD len = GetFinalPathNameByHandleW(handle, nullptr, 0,
                                        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  if (len == 0)
    return std::wstring();

  std::wstring path(len, L'\0');
  DWORD written = GetFinalPathNameByHandleW(
      handle, &path[0], len, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  if (written == 0 || written >= len)
    return std::wstring();

  path.resize(written);
  return path;
}

} // namespace

// A file action waiting to be delivered to a listener. We gather these while
// holding `mapMutex` and deliver them after releasing it.
struct ReadDirectoryChangesFileWatcher::Event {
  pathwatcher::FileWatchListener *listener;
  pathwatcher::WatchID handle;
  std::string dir;
  std::string filename;
  pathwatcher::Action action;
  std::string oldFilename;
};

struct ReadDirectoryChangesFileWatcher::Watch {
  // The kernel writes into `overlapped` and `buffer` when an operation
  // completes, so neither may be freed while `inFlight` is true. See
  // `retire()`.
  OVERLAPPED overlapped{};
  // `DWORD` elements because `ReadDirectoryChangesW` requires a DWORD-aligned
  // buffer.
  std::vector<DWORD> buffer = std::vector<DWORD>(kBufferBytes / sizeof(DWORD));

  HANDLE dirHandle = INVALID_HANDLE_VALUE;
  pathwatcher::WatchID handle = 0;
  std::string dir; // UTF-8; always ends with '\\'
  pathwatcher::FileWatchListener *listener = nullptr;

  // The path `dirHandle` resolved to when the watch was created. We compare
  // it against the handle's current path on each completion so that we can
  // tell when the directory has been renamed or moved out from under us.
  std::wstring canonicalPath;

  // Whether a `ReadDirectoryChangesW` is outstanding.
  bool inFlight = false;
  // Whether `removeWatch()` has given this watch up. A retired watch that's
  // still in flight is freed by the completion thread.
  bool retired = false;

  // The first half of a rename, waiting for its `RENAMED_NEW_NAME` partner.
  bool hasPendingOldName = false;
  std::string pendingOldName;

  // The most recent `Modified` we reported, along with the file's last-write
  // time and size at that moment. Windows tends to report a single write as
  // several `FILE_ACTION_MODIFIED`s (one for the data, one for the metadata),
  // and this lets us collapse them.
  bool hasLastModified = false;
  std::string lastModifiedName;
  ULONGLONG lastModifiedTime = 0;
  ULONGLONG lastModifiedSize = 0;

  bool isDuplicateModification(const std::string &filename) {
    ULONGLONG time = 0;
    ULONGLONG size = 0;
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (GetFileAttributesExW(ToWidePath(dir + filename).c_str(),
                             GetFileExInfoStandard, &data)) {
      time = ((ULONGLONG)data.ftLastWriteTime.dwHighDateTime << 32) |
             data.ftLastWriteTime.dwLowDateTime;
      size = ((ULONGLONG)data.nFileSizeHigh << 32) | data.nFileSizeLow;
    }

    bool isDuplicate = hasLastModified && lastModifiedName == filename &&
                       lastModifiedTime == time && lastModifiedSize == size;

    hasLastModified = true;
    lastModifiedName = filename;
    lastModifiedTime = time;
    lastModifiedSize = size;
    return isDuplicate;
  }

  // Translates one `FILE_NOTIFY_INFORMATION` record into zero or more events.
  void translate(DWORD action, const std::string &filename,
                 std::vector<Event> &events) {
    auto emit = [&](const std::string &name, pathwatcher::Action fwAction,
                    const std::string &oldName = "") {
      events.push_back({listener, handle, dir, name, fwAction, oldName});
    };

    switch (action) {
    case FILE_ACTION_ADDED:
      emit(filename, pathwatcher::Actions::Add);
      break;
    case FILE_ACTION_REMOVED:
      emit(filename, pathwatcher::Actions::Delete);
      break;
    case FILE_ACTION_MODIFIED:
      if (!isDuplicateModification(filename))
        emit(filename, pathwatcher::Actions::Modified);
      break;
    case FILE_ACTION_RENAMED_OLD_NAME:
      // Hold on to this until its partner arrives. A rename out of the
      // watched directory shows up as `FILE_ACTION_REMOVED` rather than as an
      // unpaired old name, so in practice every old name gets a partner.
      hasPendingOldName = true;
      pendingOldName = filename;
      break;
    case FILE_ACTION_RENAMED_NEW_NAME:
      if (hasPendingOldName) {
        // A rename within the watched directory — often an atomic save.
        emit(filename, pathwatcher::Actions::Moved, pendingOldName);
        hasPendingOldName = false;
      } else {
        emit(filename, pathwatcher::Actions::Add);
      }
      break;
    default:
      break;
    }
  }
};

ReadDirectoryChangesFileWatcher::ReadDirectoryChangesFileWatcher() {
  iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
  if (iocp == nullptr) {
    isValid = false;
    return;
  }

  completionThread =
      std::thread(&ReadDirectoryChangesFileWatcher::completionLoop, this);
}

ReadDirectoryChangesFileWatcher::~ReadDirectoryChangesFileWatcher() {
  isValid = false;
  if (iocp == nullptr)
    return;

  {
    std::lock_guard<std::mutex> lock(mapMutex);
    for (auto &entry : handlesToWatches)
      retire(entry.second);
    handlesToWatches.clear();
    stopping = true;
  }

  // Every watch we just retired will produce a completion packet that wakes
  // the completion thread. But if nothing was in flight, the thread is
  // blocked with nothing coming, so wake it ourselves.
  PostQueuedCompletionStatus((HANDLE)iocp, 0, 0, nullptr);

  if (completionThread.joinable())
    completionThread.join();

  // Ordinarily `retiredWatches` is empty by now: the completion thread
  // doesn't exit until every operation has completed. If the completion port
  // failed and the thread exited early, then anything left in it may still
  // have an operation the kernel will complete someday, so we leak it rather
  // than free memory the kernel might write to.
  CloseHandle((HANDLE)iocp);
}

pathwatcher::WatchID
ReadDirectoryChangesFileWatcher::addWatch(const std::string &path,
                                          pathwatcher::FileWatchListener *listener,
                                          bool /* _useRecursion */) {
#ifdef DEBUG
  std::cout << "ReadDirectoryChangesFileWatcher::addWatch: " << path
            << std::endl;
#endif
  if (!isValid)
    return pathwatcher::Errors::WatcherFailed;

  std::wstring widePath = ToWidePath(path);

  DWORD attributes = GetFileAttributesW(widePath.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    return GetLastError() == ERROR_ACCESS_DENIED
               ? pathwatcher::Errors::FileNotReadable
               : pathwatcher::Errors::FileNotFound;
  }
  if (!(attributes & FILE_ATTRIBUTE_DIRECTORY))
    return pathwatcher::Errors::FileNotFound;

  // `FILE_SHARE_DELETE` so that we don't prevent anyone from renaming or
  // deleting the directory we're watching.
  HANDLE dirHandle = CreateFileW(
      widePath.c_str(), FILE_LIST_DIRECTORY,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
      nullptr);
  if (dirHandle == INVALID_HANDLE_VALUE) {
    switch (GetLastError()) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
      return pathwatcher::Errors::FileNotFound;
    case ERROR_ACCESS_DENIED:
      return pathwatcher::Errors::FileNotReadable;
    default:
      return pathwatcher::Errors::WatcherFailed;
    }
  }

  Watch *watch = new Watch();
  watch->dirHandle = dirHandle;
  watch->listener = listener;
  watch->dir = path;
  if (watch->dir.empty() || watch->dir.back() != '\\')
    watch->dir += '\\';
  watch->canonicalPath = GetHandlePath(dirHandle);

  // The completion key is the watch itself. That's safe to dereference when a
  // packet arrives, because a watch is never freed while an operation is in
  // flight — and every packet corresponds to an operation.
  if (CreateIoCompletionPort(dirHandle, (HANDLE)iocp,
                             reinterpret_cast<ULONG_PTR>(watch),
                             0) == nullptr) {
    destroy(watch);
    return pathwatcher::Errors::WatcherFailed;
  }

  // We hold `mapMutex` while arming the watch so that, if a completion packet
  // arrives immediately, the completion thread can't process it until we've
  // finished registering the watch.
  std::lock_guard<std::mutex> lock(mapMutex);

  if (!arm(watch)) {
    destroy(watch);
    return pathwatcher::Errors::WatcherFailed;
  }

  watch->handle = nextHandleID++;
  handlesToWatches[watch->handle] = watch;
  return watch->handle;
}

void ReadDirectoryChangesFileWatcher::removeWatch(pathwatcher::WatchID handle) {
  std::lock_guard<std::mutex> lock(mapMutex);
  auto it = handlesToWatches.find(handle);
  if (it == handlesToWatches.end())
    return;
  Watch *watch = it->second;
  handlesToWatches.erase(it);
  retire(watch);
}

bool ReadDirectoryChangesFileWatcher::arm(Watch *watch) {
  watch->overlapped = OVERLAPPED{};
  BOOL ok = ReadDirectoryChangesW(watch->dirHandle, watch->buffer.data(),
                                  kBufferBytes, FALSE, kNotifyFilter, nullptr,
                                  &watch->overlapped, nullptr);
  if (!ok) {
#ifdef DEBUG
    std::cout << "ReadDirectoryChangesFileWatcher: couldn't arm watch for "
              << watch->dir << " (error " << GetLastError() << ")"
              << std::endl;
#endif
    return false;
  }

  watch->inFlight = true;
  operationsInFlight++;
  return true;
}

void ReadDirectoryChangesFileWatcher::goDormant(Watch *watch) {
  if (watch->dirHandle != INVALID_HANDLE_VALUE) {
    CloseHandle(watch->dirHandle);
    watch->dirHandle = INVALID_HANDLE_VALUE;
  }
}

void ReadDirectoryChangesFileWatcher::retire(Watch *watch) {
  if (!watch->inFlight) {
    destroy(watch);
    return;
  }

  watch->retired = true;
  retiredWatches.insert(watch);

  // This only *requests* cancellation. The operation completes some time
  // later, and its completion packet is how we learn that the kernel is done
  // with `watch`. If the operation has already completed, this fails with
  // `ERROR_NOT_FOUND` — but then its packet is already queued, so the outcome
  // is the same.
  CancelIoEx(watch->dirHandle, &watch->overlapped);
}

void ReadDirectoryChangesFileWatcher::destroy(Watch *watch) {
  if (watch->dirHandle != INVALID_HANDLE_VALUE)
    CloseHandle(watch->dirHandle);
  delete watch;
}

void ReadDirectoryChangesFileWatcher::completionLoop() {
  for (;;) {
    {
      std::lock_guard<std::mutex> lock(mapMutex);
      if (stopping && operationsInFlight == 0)
        break;
    }

    DWORD bytes = 0;
    ULONG_PTR key = 0;
    OVERLAPPED *overlapped = nullptr;
    BOOL ok = GetQueuedCompletionStatus((HANDLE)iocp, &bytes, &key,
                                        &overlapped, INFINITE);
    DWORD error = ok ? ERROR_SUCCESS : GetLastError();

    if (overlapped == nullptr) {
      if (!ok) {
        // The completion port itself has failed, so nothing more will arrive.
        isValid = false;
        break;
      }
      // A wakeup from the destructor.
      continue;
    }

    Watch *watch = reinterpret_cast<Watch *>(key);
    std::vector<Event> events;
    {
      std::lock_guard<std::mutex> lock(mapMutex);
      operationsInFlight--;
      watch->inFlight = false;

      if (watch->retired) {
        // This is the final packet for a watch that `removeWatch()` gave up;
        // the kernel is done with it, so now we can free it.
        retiredWatches.erase(watch);
        destroy(watch);
        continue;
      }

      handleCompletion(watch, ok != FALSE, bytes, error, events);
    }

    // Deliver events without holding the lock. Everything we need has been
    // copied into `events`: once we release the lock, `removeWatch()` may
    // free `watch`.
    for (auto &event : events) {
      event.listener->handleFileAction(event.handle, event.dir, event.filename,
                                       event.action, event.oldFilename);
    }
  }
}

void ReadDirectoryChangesFileWatcher::handleCompletion(
    Watch *watch, bool succeeded, unsigned long bytes, unsigned long error,
    std::vector<Event> &events) {
  if (!succeeded) {
    if (error == ERROR_NOTIFY_ENUM_DIR) {
      // Too many changes to fit in the buffer; they're lost. Keep watching.
      if (!arm(watch))
        goDormant(watch);
      return;
    }
    // Most likely `ERROR_ACCESS_DENIED`, meaning the directory has been
    // deleted. Nothing more will arrive, so let go of the directory; holding
    // it open would keep its deletion pending.
#ifdef DEBUG
    std::cout << "ReadDirectoryChangesFileWatcher: watch for " << watch->dir
              << " failed (error " << error << ")" << std::endl;
#endif
    goDormant(watch);
    return;
  }

  // The directory handle stays valid when the directory is renamed or moved,
  // so we'd keep hearing about changes to its children — but every path we
  // report is built from the directory's original location. Once the handle
  // resolves somewhere else, those paths would name files that aren't there,
  // so we stop watching instead.
  if (!watch->canonicalPath.empty() &&
      watch->canonicalPath != GetHandlePath(watch->dirHandle)) {
    goDormant(watch);
    return;
  }

  if (bytes == 0) {
    // The buffer overflowed and the kernel discarded its contents. There's
    // nothing to parse, and no way to know what we missed. Keep watching.
    if (!arm(watch))
      goDormant(watch);
    return;
  }

  // Parse everything out of the buffer before re-arming, since re-arming
  // hands the buffer back to the kernel.
  const BYTE *base = reinterpret_cast<const BYTE *>(watch->buffer.data());
  const size_t headerBytes = offsetof(FILE_NOTIFY_INFORMATION, FileName);
  size_t offset = 0;
  for (;;) {
    if (offset + headerBytes > bytes)
      break;
    auto *info =
        reinterpret_cast<const FILE_NOTIFY_INFORMATION *>(base + offset);
    if (offset + headerBytes + info->FileNameLength > bytes)
      break;

    std::string filename =
        WideToUtf8(info->FileName, info->FileNameLength / sizeof(WCHAR));
    watch->translate(info->Action, filename, events);

    if (info->NextEntryOffset == 0)
      break;
    offset += info->NextEntryOffset;
  }

  if (!arm(watch))
    goDormant(watch);
}
