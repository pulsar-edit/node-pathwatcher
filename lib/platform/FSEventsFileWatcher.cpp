#include "../core.h"
#include <sys/stat.h>
#include "FSEventsFileWatcher.hpp"

#ifdef DEBUG
#include <iostream>
#endif

// This is an API-compatible replacement for `efsw::FileWatcher` on macOS. It
// uses its own implementation of `FSEvents` so it can minimize the number of
// streams created in comparison to `efsw`’s approach of using one stream per
// watched path.

// NOTE: Lots of these utility functions are duplications and alternate
// versions of functions that are already present in `efsw`. We could use the
// `efsw` versions instead, but it feels like a good idea to minimize the
// amount of cross-pollination here.

int shorthandFSEventsModified = kFSEventStreamEventFlagItemFinderInfoMod |
  kFSEventStreamEventFlagItemModified |
  kFSEventStreamEventFlagItemInodeMetaMod;

// Ensure a given path has a trailing separator for comparison purposes.
static std::string NormalizePath(std::string path) {
  if (path.back() == PATH_SEPARATOR) return path;
  return path + PATH_SEPARATOR;
}

static bool PathsAreEqual(std::string pathA, std::string pathB) {
  return NormalizePath(pathA) == NormalizePath(pathB);
}

std::string PrecomposeFileName(const std::string& name) {
	CFStringRef cfStringRef = CFStringCreateWithCString(
    kCFAllocatorDefault,
    name.c_str(),
    kCFStringEncodingUTF8
  );

	CFMutableStringRef cfMutable = CFStringCreateMutableCopy(NULL, 0, cfStringRef);

	CFStringNormalize(cfMutable, kCFStringNormalizationFormC);

	const char* c_str = CFStringGetCStringPtr(cfMutable, kCFStringEncodingUTF8);

	if (c_str != NULL) {
		std::string result(c_str);
		CFRelease(cfStringRef);
		CFRelease(cfMutable);
		return result;
	}

	CFIndex length = CFStringGetLength(cfMutable);
	CFIndex maxSize = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8);
	if (maxSize == kCFNotFound) {
		CFRelease(cfStringRef);
		CFRelease(cfMutable);
		return std::string();
	}

	std::string result(maxSize + 1, '\0');
	if (CFStringGetCString(
    cfMutable,
    &result[0],
    result.size(),
    kCFStringEncodingUTF8
  )) {
		result.resize(std::strlen(result.c_str()));
		CFRelease(cfStringRef);
		CFRelease(cfMutable);
	} else {
		result.clear();
	}
	return result;
}

// Returns whether `path` currently exists on disk. Does not distiguish between
// files and directories.
bool PathExists(const std::string& path) {
  struct stat buffer;
  return (stat(path.c_str(), &buffer) == 0);
}

// Given two paths, determine whether the first descends from (or is equal to)
// the second.
bool PathStartsWith(const std::string& str, const std::string& prefix) {
  if (PathsAreEqual(str, prefix)) return true;
  if (prefix.length() > str.length()) {
    return false;
  }
  // We ensure `prefix` ends with a path separator so we don't mistakenly think
  // that `/foo/barbaz` descends from `/foo/bar`.
  auto normalizedPrefix = NormalizePath(prefix);
  return str.compare(0, normalizedPrefix.length(), normalizedPrefix) == 0;
}

// Strips a trailing slash from the path (in place).
void DirRemoveSlashAtEnd (std::string& dir) {
  if (dir.size() >= 1 && dir[dir.size() - 1] == PATH_SEPARATOR) {
    dir.erase( dir.size() - 1 );
  }
}

// Given `/foo/bar/baz.txt`, returns `/foo/bar` (or `/foo/bar/`).
//
// Given `/foo/bar/baz`, also returns `/foo/bar` (or `/foo/bar/`). In other
// words: it works like Node’s `path.dirname` and strips the last segment of a
// path.
std::string PathWithoutFileName(std::string filepath, bool keepTrailingSeparator) {
  DirRemoveSlashAtEnd(filepath);

  size_t pos = filepath.find_last_of(PATH_SEPARATOR);
  if (pos != std::string::npos) {
    return filepath.substr(0, keepTrailingSeparator ? pos + 1 : pos);
  }
  return filepath;
}

std::string PathWithoutFileName(std::string filepath) {
  // Default behavior of `PathWithoutFileName` is to keep the trailing
  // separator.
  return PathWithoutFileName(filepath, true);
}

// Given `/foo/bar/baz.txt`, returns `baz.txt`.
//
// Given `/foo/bar/baz`, returns `baz`.
//
// Equivalent to Node’s `path.basename`.
std::string FileNameFromPath(std::string filepath) {
  DirRemoveSlashAtEnd(filepath);

  size_t pos = filepath.find_last_of(PATH_SEPARATOR);

	if (pos != std::string::npos) {
		return filepath.substr(pos + 1);
	}
	return filepath;
}

// Borrowed from `efsw`. Don’t ask me to explain it.
static std::string convertCFStringToStdString( CFStringRef cfString ) {
  // Try to get the C string pointer directly.
	const char* cStr = CFStringGetCStringPtr(cfString, kCFStringEncodingUTF8);

	if (cStr) {
    // If the pointer is valid, directly return a `std::string` from it.
		return std::string(cStr);
	} else {
    // If not, manually convert it.
		CFIndex length = CFStringGetLength(cfString);
		CFIndex maxSize = CFStringGetMaximumSizeForEncoding(
      length,
      kCFStringEncodingUTF8
    ) + 1; // +1 for null terminator

		char* buffer = new char[maxSize];

		if (CFStringGetCString(
      cfString,
      buffer,
      maxSize,
      kCFStringEncodingUTF8
    )) {
			std::string result(buffer);
			delete[] buffer;
			return result;
		} else {
			delete[] buffer;
			return "";
		}
	}
}

// Empty constructor.

FSEventsFileWatcher::~FSEventsFileWatcher() {
  pendingDestruction = true;
  // Defer cleanup until we can finish processing file events.
  std::unique_lock<std::mutex> lock(processingMutex);
  while (isProcessing) {
    processingComplete.wait(lock);
  }

  isValid = false;
  if (currentEventStream) {
    FSEventStreamStop(currentEventStream);
    FSEventStreamInvalidate(currentEventStream);
    FSEventStreamRelease(currentEventStream);
  }
}

efsw::WatchID FSEventsFileWatcher::addWatch(
  const std::string& directory,
  efsw::FileWatchListener* listener,
  // The `_useRecursion` flag is ignored; it's present for API compatibility.
  bool _useRecursion
) {
  efsw::WatchID handle = nextHandleID++;
  {
    std::lock_guard<std::mutex> lock(mapMutex);
    handlesToPaths[handle] = directory;
    pathsToHandles[directory] = handle;
    handlesToListeners[handle] = listener;
  }

  bool didStart = startNewStream();

  if (!didStart) {
    removeHandle(handle);
    // TODO: Debug information?
    return -handle;
  }

  return handle;
}

void FSEventsFileWatcher::removeWatch(
  efsw::WatchID handle
) {
  auto remainingCount = removeHandle(handle);

  if (remainingCount == 0) {
    if (currentEventStream) {
      FSEventStreamStop(currentEventStream);
      FSEventStreamInvalidate(currentEventStream);
      FSEventStreamRelease(currentEventStream);
    }
    currentEventStream = nullptr;
    return;
  }

  // We don't capture the return value here because it doesn’t affect our
  // response. If a new stream fails to start for whatever reason, the old
  // stream will still work. And because we've removed the handle from the
  // relevant maps, we will silently ignore any filesystem events that happen
  // at the given path.
  startNewStream();
}

void FSEventsFileWatcher::FSEventCallback(
  ConstFSEventStreamRef streamRef,
  void* userData,
  size_t numEvents,
  void* eventPaths,
  const FSEventStreamEventFlags eventFlags[],
  const FSEventStreamEventId eventIds[]
) {
  FSEventsFileWatcher* instance = static_cast<FSEventsFileWatcher*>(userData);
  if (!instance->isValid) return;

  std::vector<FSEvent> events;
  events.reserve(numEvents);

  for (size_t i = 0; i < numEvents; i++) {
    CFDictionaryRef pathInfoDict = static_cast<CFDictionaryRef>(
      CFArrayGetValueAtIndex((CFArrayRef) eventPaths, i)
    );
    CFStringRef path = static_cast<CFStringRef>(
      CFDictionaryGetValue(pathInfoDict, kFSEventStreamEventExtendedDataPathKey)
    );
    CFNumberRef cfInode = static_cast<CFNumberRef>(
      CFDictionaryGetValue(pathInfoDict, kFSEventStreamEventExtendedFileIDKey)
    );

    if (cfInode) {
      unsigned long inode = 0;
      CFNumberGetValue(cfInode, kCFNumberLongType, &inode);
      events.push_back(
        FSEvent(
          convertCFStringToStdString(path),
          (long) eventFlags[i],
          (uint64_t) eventIds[i],
          inode
        )
      );
    }
  }

  if (!instance->isValid) return;
  instance->handleActions(events);
  instance->process();
}

void FSEventsFileWatcher::sendFileAction(
  efsw::WatchID watchid,
  const std::string& dir,
  const std::string& filename,
  efsw::Action action,
  std::string oldFilename
) {
  efsw::FileWatchListener* listener;
  auto it = handlesToListeners.find(watchid);
  if (it == handlesToListeners.end()) return;
  listener = it->second;

  listener->handleFileAction(
    watchid,
    PrecomposeFileName(dir),
    PrecomposeFileName(filename),
    action,
    PrecomposeFileName(oldFilename)
  );
}

struct FileEventMatch {
  efsw::WatchID handle;
  std::string path;
};

void FSEventsFileWatcher::handleActions(std::vector<FSEvent>& events) {
  size_t esize = events.size();

  for (size_t i = 0; i < esize; i++) {
    FSEvent& event = events[i];
    std::vector<FileEventMatch> matches;

    if (event.flags & (
      kFSEventStreamEventFlagUserDropped |
      kFSEventStreamEventFlagKernelDropped |
      kFSEventStreamEventFlagEventIdsWrapped |
      kFSEventStreamEventFlagHistoryDone |
      kFSEventStreamEventFlagMount |
      kFSEventStreamEventFlagUnmount |
      kFSEventStreamEventFlagRootChanged
    )) continue;

    efsw::WatchID handle;
    std::string path;

    {
      // How do we match up this path change to the watcher that cares about
      // it?
      //
      // Since we do only non-recursive watching, each filesystem event can
      // belong to one watcher at most. This vastly simplifies our
      // implementation compared to `efsw`’s — since it has to care about the
      // possibility of recursive watchers, one file change can correspond to
      // arbitrarily many watchers.
      //
      // For that reason, we can do a simple map lookup. First we try the
      // path’s parent directory; if that’s not successful, we try the full
      // path. One of these is (for practical purposes) guaranteed to find a
      // watcher.
      //
      // NOTE: `efsw` currently does not detect a directory’s deletion when
      // that directory is the one being watched. For consistency, we'll try to
      // make this custom `FileWatcher` instance behave the same way.
      //
      // This works in our favor because it means that there can be only one
      // watcher responding to this filesystem event. The only way to find
      // lifecycle events on directories themselves — deletions, renames,
      // creations — is to listen on the directory’s parent, which neatly
      // mirrors the situation with files.
      //
      std::lock_guard<std::mutex> lock(mapMutex);
      auto itpth = pathsToHandles.find(PathWithoutFileName(event.path, false));
      if (itpth != pathsToHandles.end()) {
        // We have an entry for this paths’s owner directory. We prefer this
        // whether the entry itself is a file or a directory (to replicate
        // `efsw`’s bug).
        path = itpth->first;
        handle = itpth->second;
      } else {
        // Couldn't match this up to a watcher. A bit unusual, but not
        // catastrophic.
        continue;
      }
    }

    std::string dirPath(PathWithoutFileName(event.path));
    std::string filePath(FileNameFromPath(event.path));

    if (event.flags & (
      kFSEventStreamEventFlagItemCreated |
      kFSEventStreamEventFlagItemRemoved |
      kFSEventStreamEventFlagItemRenamed
    )) {
      if (!PathsAreEqual(dirPath, path)) {
        dirsChanged.insert(dirPath);
      }
    }

    // `efsw`‘s comment here suggests that you can’t reliably infer order from
    // these events — so if the same file is marked as added and changed and
    // deleted in consecutive events, you don't know if it was deleted/
    // added/modified, modified/deleted/added, etc.
    //
    // This is the equivalent logic from `WatcherFSEvents.cpp` because I don’t
    // trust myself to touch it at all. The goal is largely to infer an
    // ordering to the extent possible based on whether the path exists at the
    // moment.
    if (event.flags & kFSEventStreamEventFlagItemRenamed) {
      // Does the next event also refer to this same file, and is that event
      // also a rename?
      if (
        (i + 1 < esize) &&
        (events[i + 1].flags & kFSEventStreamEventFlagItemRenamed) &&
        (events[i + 1].inode == event.inode)
      ) {
        // If so, compare this event and the next one to figure out which one
        // refers to a current file on disk.
        FSEvent& nEvent = events[i + 1];
        std::string newDir(PathWithoutFileName(nEvent.path));
        std::string newFilepath(FileNameFromPath(nEvent.path));

        if (event.path != nEvent.path) {
          if (dirPath == newDir) {
            // This is a move within the same directory.
            if (
              !PathExists(event.path) ||
              0 == strcasecmp(event.path.c_str(), nEvent.path.c_str())
            ) {
              // Move from one path to the other.
              sendFileAction(handle, dirPath, newFilepath, efsw::Actions::Moved, filePath);
            } else {
              // Move in the opposite direction.
              sendFileAction(handle, dirPath, filePath, efsw::Actions::Moved, newFilepath);
            }
          } else {
            // This is a move from one directory to another, so we'll treat
            // it as one deletion and one creation.
            sendFileAction(handle, dirPath, filePath, efsw::Actions::Delete);
            sendFileAction(handle, newDir, newFilepath, efsw::Actions::Add);

            if (nEvent.flags & shorthandFSEventsModified) {
              sendFileAction(handle, dirPath, filePath, efsw::Actions::Modified);
            }
          }
        } else {
          // The file paths are the same, so we'll let another function
          // untangle it.
          handleAddModDel(handle, nEvent.flags, nEvent.path, dirPath, filePath);
        }

        if (nEvent.flags & (
          kFSEventStreamEventFlagItemCreated |
          kFSEventStreamEventFlagItemRemoved |
          kFSEventStreamEventFlagItemRenamed
        )) {
          if (!PathsAreEqual(newDir, path)) {
            dirsChanged.insert(newDir);
          }
        }

        // Skip the renamed file.
        i++;
      } else if (PathExists(event.path)) {
        // Treat remaining renames as creations when we know the path still
        // exists…
        sendFileAction(handle, dirPath, filePath, efsw::Actions::Add);

        if (event.flags & shorthandFSEventsModified) {
          sendFileAction(handle, dirPath, filePath, efsw::Actions::Modified);
        }
      } else {
        // …and as deletions when we know the path doesn’t still exist.
        sendFileAction(handle, dirPath, filePath, efsw::Actions::Delete);
      }
    } else {
      // Ordinary business — new files, changed, files, deleted files.
      handleAddModDel(handle, event.flags, event.path, dirPath, filePath);
    }
  }
}

void FSEventsFileWatcher::handleAddModDel(
  efsw::WatchID handle,
  const uint32_t& flags,
  const std::string& path,
  std::string& dirPath,
  std::string& filePath
) {
  if (flags & kFSEventStreamEventFlagItemCreated) {
    // This claims to be a file creation; make sure it exists on disk before
    // triggering an event.
    if (PathExists(path)) {
      sendFileAction(handle, dirPath, filePath, efsw::Actions::Add);
    }
  }

  if (flags & shorthandFSEventsModified) {
    sendFileAction(handle, dirPath, filePath, efsw::Actions::Modified);
  }

  if (flags & kFSEventStreamEventFlagItemRemoved) {
    // This claims to be a file deletion; make sure it doesn't exist on disk
    // before triggering an event.
    if (!PathExists(path)) {
      sendFileAction(handle, dirPath, filePath, efsw::Actions::Delete);
    }
  }
}

// Private: clean up a handle from both unordered maps.
size_t FSEventsFileWatcher::removeHandle(efsw::WatchID handle) {
  std::lock_guard<std::mutex> lock(mapMutex);
  std::string path;
  auto itp = handlesToPaths.find(handle);
  if (itp != handlesToPaths.end()) {
    path = itp->second;
    handlesToPaths.erase(itp);
  }
  auto itpth = pathsToHandles.find(path);
  if (itpth != pathsToHandles.end()) {
    pathsToHandles.erase(itpth);
  }
  auto itl = handlesToListeners.find(handle);
  if (itl != handlesToListeners.end()) {
    handlesToListeners.erase(itl);
  }
  return handlesToPaths.size();
}

void FSEventsFileWatcher::process() {
  // We are very careful in this function to ensure that `FSEventsFileWatcher`
  // doesn’t finalize while this is happening.
  if (!isValid || pendingDestruction) return;
  {
    std::unique_lock<std::mutex> lock(processingMutex);
    if (isProcessing) return;
    isProcessing = true;
  }

  ProcessingGuard guard(*this);

  std::set<std::string> dirsCopy;
  {
    dirsCopy = dirsChanged;
    dirsChanged.clear();
  }

  // Process the copied directories.
  for (const auto& dir : dirsCopy) {
    if (pendingDestruction) return;

    efsw::WatchID handle;
    std::string path;

    {
      std::lock_guard<std::mutex> lock(mapMutex);
      auto itpth = pathsToHandles.find(PathWithoutFileName(dir, false));
      if (itpth == pathsToHandles.end()) continue;
      path = itpth->first;
      handle = itpth->second;
    }

    // TODO: It is questionable whether these file events are useful or
    // actionable, since the listener will fail to respond to them if they come
    // from an unexpected path on disk.
    sendFileAction(
      handle,
      PathWithoutFileName(dir),
      FileNameFromPath(dir),
      efsw::Actions::Modified
    );

    if (pendingDestruction) return;
  }
}

// Start a new FSEvent stream and promote it to the “active” stream after it
// starts.
bool FSEventsFileWatcher::startNewStream() {
  // Build a list of all current watched paths. We'll eventually pass this to
  // `FSEventStreamCreate`.
  std::vector<CFStringRef> cfStrings;
  {
    std::lock_guard<std::mutex> lock(mapMutex);
    for (const auto& pair : handlesToPaths) {
      CFStringRef cfStr = CFStringCreateWithCString(
        kCFAllocatorDefault,
        pair.second.c_str(),
        kCFStringEncodingUTF8
      );
      if (cfStr) {
        cfStrings.push_back(cfStr);
      }
    }
  }

  CFArrayRef paths = CFArrayCreate(
    NULL,
    reinterpret_cast<const void**>(cfStrings.data()),
    cfStrings.size(),
    NULL
  );

  uint32_t streamFlags = kFSEventStreamCreateFlagNone;

  streamFlags = kFSEventStreamCreateFlagFileEvents |
    kFSEventStreamCreateFlagNoDefer |
    kFSEventStreamCreateFlagUseExtendedData |
    kFSEventStreamCreateFlagUseCFTypes;

  FSEventStreamContext ctx;
  ctx.version = 0;
  ctx.info = this;
  ctx.retain = NULL;
  ctx.release = NULL;
  ctx.copyDescription = NULL;

  dispatch_queue_t queue = dispatch_queue_create(NULL, NULL);

  nextEventStream = FSEventStreamCreate(
    kCFAllocatorDefault,
    &FSEventsFileWatcher::FSEventCallback,
    &ctx,
    paths,
    kFSEventStreamEventIdSinceNow,
    0.,
    streamFlags
  );

  FSEventStreamSetDispatchQueue(nextEventStream, queue);
  bool didStart = FSEventStreamStart(nextEventStream);

  // Release all the strings we just created.
  for (CFStringRef str : cfStrings) {
    CFRelease(str);
  }
  CFRelease(paths);

  // If it started successfully, we can swap it into place as the new main
  // stream.
  if (didStart) {
    if (currentEventStream) {
      FSEventStreamStop(currentEventStream);
      FSEventStreamInvalidate(currentEventStream);
      FSEventStreamRelease(currentEventStream);
    }
    currentEventStream = nextEventStream;
    nextEventStream = nullptr;
  }

  return didStart;
}
