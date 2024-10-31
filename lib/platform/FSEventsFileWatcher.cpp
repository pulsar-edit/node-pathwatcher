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

// NOTE: Lots of these are duplications and alternate versions of functions
// that are already present in `efsw`. We could use the `efsw` versions
// instead, but it feels like a good idea to minimize the amount of
// cross-pollination here.

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

bool PathExists(const std::string& path) {
  struct stat buffer;
  return (stat(path.c_str(), &buffer) == 0);
}

bool PathStartsWith(const std::string& str, const std::string& prefix) {
  if (PathsAreEqual(str, prefix)) return true;
  if (prefix.length() > str.length()) {
    return false;
  }
  return str.compare(0, prefix.length(), prefix) == 0;
}

void DirRemoveSlashAtEnd (std::string& dir) {
  if (dir.size() >= 1 && dir[dir.size() - 1] == PATH_SEPARATOR) {
    dir.erase( dir.size() - 1 );
  }
}

std::string PathWithoutFileName( std::string filepath ) {
  DirRemoveSlashAtEnd(filepath);

  size_t pos = filepath.find_last_of(PATH_SEPARATOR);

  if (pos != std::string::npos) {
    return filepath.substr(0, pos + 1);
  }
  return filepath;
}

std::string FileNameFromPath(std::string filepath) {
  DirRemoveSlashAtEnd(filepath);

  size_t pos = filepath.find_last_of(PATH_SEPARATOR);

	if (pos != std::string::npos) {
		return filepath.substr(pos + 1);
	}
	return filepath;
}

static std::string convertCFStringToStdString( CFStringRef cfString ) {
	// Try to get the C string pointer directly
	const char* cStr = CFStringGetCStringPtr( cfString, kCFStringEncodingUTF8 );

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
#ifdef DEBUG
  std::cout << "[destroying] FSEventsFileWatcher!" << std::endl;
#endif
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
#ifdef DEBUG
  std::cout << "FSEventsFileWatcher::addWatch" << directory << std::endl;
#endif
  efsw::WatchID handle = nextHandleID++;
  handlesToPaths[handle] = directory;
  handlesToListeners[handle] = listener;

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
#ifdef DEBUG
  std::cout << "FSEventsFileWatcher::removeWatch" << handle << std::endl;
#endif
  removeHandle(handle);

  if (handlesToPaths.size() == 0) {
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
#ifdef DEBUG
  std::cout << "FSEventsFileWatcher::FSEventCallback" << std::endl;
#endif

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

    // Since we aren’t doing recursive watchers, we can seemingly get away with
    // using the first match we find. In nearly all cases, it’s impossible for
    // a filesystem event to pertain to more than one watcher.
    //
    // But one exception is directory deletion, since that directory _and_ its
    // parent directory can both be watched, and that would count as a deletion
    // for the first and a change for the second.
    //
    // For that reason, we keep an array of matches, but stop as soon as we
    // find two matches, since that's the practical maximum.
    //
    // TODO: On other platforms we handle this in the listener’s
    // `handleFileAction` method. We don’t handle it there on macOS because the
    // behavior of this function means we don’t know which of the two watchers
    // in this scenario will be matched first. We could instead consistently
    // return one or the other here, but since we’d have to loop through all
    // the options anyway, it wouldn’t save us much effort.
    //
    // NOTE: In extreme situations with lots of paths, this could be a choke
    // point, since it’s a nested loop. The fact that we’re just doing string
    // comparisons should keep it fast, but we could optimize further by
    // pre-normalizing the paths. We could also move to a better data
    // structure, but better to invest that time in making this library
    // obsolete in the first place.
    //
    // TODO: We could probably do this without looping somehow, right?
    for (const auto& pair: handlesToPaths) {
      std::string normalizedPath = NormalizePath(pair.second);

      // Filter out everything that doesn’t equal or descend from this path.
      if (!PathStartsWith(event.path, normalizedPath)) continue;

      // We let this through if (a) it matches our path exactly, or (b) it
      // refers to a child file/directory of ours (rather than a deeper
      // descendant.)
      if (
        !PathsAreEqual(event.path, normalizedPath) && event.path.find_last_of(PATH_SEPARATOR) != normalizedPath.size() - 1
      ) {
        continue;
      }

      FileEventMatch match;
      match.path = pair.second;
      match.handle = pair.first;
      matches.push_back(match);

      // TODO: We can probably break after the first match in many situations.
      // For instance, if we prove this can’t be a directory deletion!
      if (matches.size() == 2) break;
    }

    if (matches.size() == 0) return;

    std::string dirPath(PathWithoutFileName(event.path));
    std::string filePath(FileNameFromPath(event.path));

    size_t msize = matches.size();

    for (size_t i = 0; i < msize; i++) {
      std::string path(matches[i].path);
      efsw::WatchID handle(matches[i].handle);

      if (event.flags & (
        kFSEventStreamEventFlagItemCreated |
        kFSEventStreamEventFlagItemRemoved |
        kFSEventStreamEventFlagItemRenamed
      )) {
        if (dirPath != path) {
          dirsChanged.insert(dirPath);
        }
      }

      // `efsw`‘s’ comment here suggests that you can’t reliably infer order
      // from these events — so if the same file is marked as added and changed
      // and deleted in consecutive events, you don't know if it was deleted/
      // added/modified, modified/deleted/added, etc.
      //
      // This is the equivalent logic from `WatcherFSEvents.cpp` because I
      // don’t trust myself to touch it at all.
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
            if (newDir != path) {
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
void FSEventsFileWatcher::removeHandle(efsw::WatchID handle) {
  auto itp = handlesToPaths.find(handle);
  if (itp != handlesToPaths.end()) {
    handlesToPaths.erase(itp);
  }
  auto itl = handlesToListeners.find(handle);
  if (itl != handlesToListeners.end()) {
    handlesToListeners.erase(itl);
  }
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

  // Process the copied directories
  for (const auto& dir : dirsCopy) {
    if (pendingDestruction) return;

    efsw::WatchID handle;
    std::string path;
    bool found = false;

    for (const auto& pair: handlesToPaths) {
      if (!PathStartsWith(dir, pair.second)) continue;

      if (
        !PathsAreEqual(dir, pair.second) && dir.find_last_of(PATH_SEPARATOR) != pair.second.size() - 1
      ) {
        continue;
      }

      found = true;
      path = pair.second;
      handle = pair.first;
      break;
    }

    if (!found) continue;

    sendFileAction(
      handle,
      PathWithoutFileName(dir),
      FileNameFromPath(dir),
      efsw::Actions::Modified
    );

    if (pendingDestruction) return;
  }

  dirsChanged.clear();
}

// Start a new FSEvent stream and promote it to the “active” stream after it
// starts.
bool FSEventsFileWatcher::startNewStream() {
  // Build a list of all current watched paths. We'll eventually pass this to
  // `FSEventStreamCreate`.
  std::vector<CFStringRef> cfStrings;
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
