#include "../core.h"
#include <sys/stat.h>
#include <iostream>
#include "FSEventsFileWatcher.hpp"

int shorthandFSEventsModified = kFSEventStreamEventFlagItemFinderInfoMod |
  kFSEventStreamEventFlagItemModified |
  kFSEventStreamEventFlagItemInodeMetaMod;

// Ensure a given path has a trailing separator for comparison purposes.
static std::string NormalizePath(std::string path) {
  if (path.back() == PATH_SEPARATOR) return path;
  return path + PATH_SEPARATOR;
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

	if ( cStr ) {
		// If the pointer is valid, directly return a std::string from it
		return std::string( cStr );
	} else {
		// If not, manually convert it
		CFIndex length = CFStringGetLength( cfString );
		CFIndex maxSize = CFStringGetMaximumSizeForEncoding( length, kCFStringEncodingUTF8 ) +
						  1; // +1 for null terminator

		char* buffer = new char[maxSize];

		if ( CFStringGetCString( cfString, buffer, maxSize, kCFStringEncodingUTF8 ) ) {
			std::string result( buffer );
			delete[] buffer;
			return result;
		} else {
			delete[] buffer;
			return "";
		}
	}
}


FSEventsFileWatcher::~FSEventsFileWatcher() {
#ifdef DEBUG
  std::cout << "[destroying] FSEventsFileWatcher!" << std::endl;
#endif
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
  bool _useRecursion
) {
#ifdef DEBUG
  std::cout << "FSEventsFileWatcher::addWatch" << directory << std::endl;
#endif
  auto handle = nextHandleID++;
  handlesToPaths.insert(handle, directory);
  std::vector<CFStringRef> cfStrings;
  for (const auto& pair: handlesToPaths.forward) {
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

  for (CFStringRef str : cfStrings) {
    CFRelease(str);
  }
  CFRelease(paths);

  if (!didStart) {
    handlesToPaths.remove(handle);
    // TODO: Debug information?
    return -handle;
  } else {
#ifdef DEBUG
    std::cout << "Started stream!" << std::endl;
#endif
    // Successfully started — we can make it the new event stream and stop the
    // old one.
    if (currentEventStream) {
      FSEventStreamStop(currentEventStream);
      FSEventStreamInvalidate(currentEventStream);
      FSEventStreamRelease(currentEventStream);
#ifdef DEBUG
    std::cout << "Stopped old stream!" << std::endl;
#endif
    }
    currentEventStream = nextEventStream;
    nextEventStream = nullptr;
  }

  handlesToListeners[handle] = listener;

  return handle;
}

void FSEventsFileWatcher::removeWatch(
  efsw::WatchID handle
) {
#ifdef DEBUG
  std::cout << "FSEventsFileWatcher::removeWatch" << handle << std::endl;
#endif
  handlesToPaths.remove(handle);

  if (handlesToPaths.forward.size() == 0) {
    handlesToListeners.erase(handle);
    if (currentEventStream) {
      FSEventStreamStop(currentEventStream);
      FSEventStreamInvalidate(currentEventStream);
      FSEventStreamRelease(currentEventStream);
    }
    currentEventStream = nullptr;
    return;
  }

  std::vector<CFStringRef> cfStrings;
  for (const auto& pair: handlesToPaths.forward) {
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

  for (CFStringRef str : cfStrings) {
    CFRelease(str);
  }
  CFRelease(paths);

  // auto it = handlesToListeners.find(handle);
  // if (it != handlesToListeners.end()) {
  //   handlesToListeners.erase(it);
  // }

  handlesToListeners.erase(handle);

  if (!didStart) {
    // We'll still remove the listener. Weird that the stream didn't stop, but
    // we can at least ignore any resulting FSEvents.
  } else {
    if (currentEventStream) {
      FSEventStreamStop(currentEventStream);
      FSEventStreamInvalidate(currentEventStream);
      FSEventStreamRelease(currentEventStream);
    }
    currentEventStream = nextEventStream;
    nextEventStream = nullptr;
  }
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

void FSEventsFileWatcher::handleActions(std::vector<FSEvent>& events) {
  size_t esize = events.size();

  for (size_t i = 0; i < esize; i++) {
    FSEvent& event = events[i];

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
    bool found = false;

    for (const auto& pair: handlesToPaths.forward) {
      std::string normalizedPath = NormalizePath(pair.second);
      if (!PathStartsWith(event.path, pair.second)) continue;
      if (event.path.find_last_of(PATH_SEPARATOR) != pair.second.size()) {
        continue;
      }
      found = true;
      path = pair.second;
      handle = pair.first;
      break;
    }

    if (!found) continue;

    std::string dirPath(PathWithoutFileName(event.path));
    std::string filePath(FileNameFromPath(event.path));

    if (event.flags & (
      kFSEventStreamEventFlagItemCreated |
      kFSEventStreamEventFlagItemRemoved |
      kFSEventStreamEventFlagItemRenamed
    )) {
      if (dirPath != path) {
        dirsChanged.insert(dirPath);
      }
    }

    if (event.flags & kFSEventStreamEventFlagItemRenamed) {
      if (
        (i + 1 < esize) &&
        (events[i + 1].flags & kFSEventStreamEventFlagItemRenamed) &&
        (events[i + 1].inode == event.inode)
      ) {
        FSEvent& nEvent = events[i + 1];
        std::string newDir(PathWithoutFileName(nEvent.path));
        std::string newFilepath(FileNameFromPath(nEvent.path));

        if (event.path != nEvent.path) {
          if (dirPath == newDir) {
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
            sendFileAction(handle, dirPath, filePath, efsw::Actions::Delete);
            sendFileAction(handle, newDir, newFilepath, efsw::Actions::Add);

            if (nEvent.flags & shorthandFSEventsModified) {
              sendFileAction(handle, dirPath, filePath, efsw::Actions::Modified);
            }
          }
        } else {
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

        // Skip the renamed file
        i++;
      } else if (PathExists(event.path)) {
        sendFileAction(handle, dirPath, filePath, efsw::Actions::Add);

        if (event.flags && shorthandFSEventsModified) {
          sendFileAction(handle, dirPath, filePath, efsw::Actions::Modified);
        }
      } else {
        sendFileAction(handle, dirPath, filePath, efsw::Actions::Delete);
      }
    } else {
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
    if (PathExists(path)) {
      sendFileAction(handle, dirPath, filePath, efsw::Actions::Add);
    }
  }

  if (flags & shorthandFSEventsModified) {
    sendFileAction(handle, dirPath, filePath, efsw::Actions::Modified);
  }

  if (flags & kFSEventStreamEventFlagItemRemoved) {
    if (!PathExists(path)) {
      sendFileAction(handle, dirPath, filePath, efsw::Actions::Delete);
    }
  }
}

void FSEventsFileWatcher::process() {
  std::lock_guard<std::mutex> lock(dirsChangedMutex);
  std::set<std::string>::iterator it = dirsChanged.begin();
  for (; it != dirsChanged.end(); it++) {

    std::string dir = *it;

    efsw::WatchID handle;
    std::string path;
    bool found = false;

    for (const auto& pair: handlesToPaths.forward) {
      // std::string normalizedPath = NormalizePath(pair.second);
      if (!PathStartsWith(dir, pair.second)) continue;
      if (dir.find_last_of(PATH_SEPARATOR) != pair.second.size()) {
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
  }

  dirsChanged.clear();
}
