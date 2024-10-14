#include <errno.h>
#include <unistd.h>
#include <sys/event.h>
#include <sys/param.h>
#include <sys/time.h>
#include <sys/types.h>
#include <thread>
#include <algorithm>
#include <fcntl.h>
#include "common.h"
#include "addon-data.h"

// test for descriptor event notification, if not available set to O_RDONLY
#ifndef O_EVTONLY
#define O_EVTONLY O_RDONLY
#endif

// test for flag to return full path of the fd
// if not then set value as defined by mac
// see: http://fxr.watson.org/fxr/source/bsd/sys/fcntl.h?v=xnu-792.6.70
#ifndef F_GETPATH
#define F_GETPATH 50

#endif

// NOTE: You might see the globals and get nervous here. Our working theory is
// that this this is fine; this is thread-safe without having to be isolated
// between contexts.
// static int g_kqueue;
// static int g_init_errno;

void PlatformInit(Napi::Env env) {
  auto addonData = env.GetInstanceData<AddonData>();
  addonData->kqueue = kqueue();
  if (addonData->kqueue == -1) {
    addonData->init_errno = errno;
    return;
  }
}

void PlatformThread(
  const PathWatcherWorker::ExecutionProgress& progress,
  bool& shouldStop,
  Napi::Env env
) {
  auto addonData = env.GetInstanceData<AddonData>();
  int l_kqueue = addonData->kqueue;
  // std::cout << "PlatformThread " << std::this_thread::get_id() << std::endl;
  struct kevent event;
  struct timespec timeout = { 0, 500000000 };

  while (!shouldStop) {
    int r;
    do {
      if (shouldStop) return;
      r = kevent(l_kqueue, NULL, 0, &event, 1, &timeout);
    } while ((r == -1 && errno == EINTR) || r == 0);

    EVENT_TYPE type;
    int fd = static_cast<int>(event.ident);
    std::vector<char> path;

    if (event.fflags & NOTE_WRITE) {
      type = EVENT_CHANGE;
    } else if (event.fflags & NOTE_DELETE) {
      type = EVENT_DELETE;
    } else if (event.fflags & NOTE_RENAME) {
      type = EVENT_RENAME;
      char buffer[MAXPATHLEN] = { 0 };
      fcntl(fd, F_GETPATH, buffer);
      close(fd);

      int length = strlen(buffer);
      path.resize(length);
      std::copy(buffer, buffer + length, path.data());
    } else if (event.fflags & NOTE_ATTRIB && lseek(fd, 0, SEEK_END) == 0) {
      // The file became empty, this does not fire as a NOTE_WRITE event for
      // some reason.
      type = EVENT_CHANGE;
    } else {
      continue;
    }

    // std::cout << "PlatformThread EVENT " << std::this_thread::get_id() << std::endl;
    PathWatcherEvent event(type, fd, path);
    progress.Send(&event, 1);
  }
}

WatcherHandle PlatformWatch(const char* path, Napi::Env env) {
  auto addonData = env.GetInstanceData<AddonData>();
  if (addonData->kqueue == -1) {
    return -addonData->init_errno;
  }

  int fd = open(path, O_EVTONLY, 0);
  if (fd < 0) {
    return -errno;
  }

  struct timespec timeout = { 0, 50000000 };
  struct kevent event;
  int filter = EVFILT_VNODE;
  int flags = EV_ADD | EV_ENABLE | EV_CLEAR;
  int fflags = NOTE_WRITE | NOTE_DELETE | NOTE_RENAME | NOTE_ATTRIB;
  EV_SET(&event, fd, filter, flags, fflags, 0, reinterpret_cast<void*>(const_cast<char*>(path)));
  int r = kevent(addonData->kqueue, &event, 1, NULL, 0, &timeout);
  if (r == -1) {
    return -errno;
  }

  return fd;
}


void PlatformUnwatch(WatcherHandle fd, Napi::Env _env) {
  close(fd);
}

bool PlatformIsHandleValid(WatcherHandle handle) {
  return handle >= 0;
}

int PlatformInvalidHandleToErrorNumber(WatcherHandle handle) {
  return -handle;
}

void PlatformStop(Napi::Env env) {}
