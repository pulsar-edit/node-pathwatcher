#include "common.h"
#pragma once

static int g_next_addon_data_id = 1;

class AddonData final {
public:
  explicit AddonData(Napi::Env env) {
    id = g_next_addon_data_id++;
  }

  Napi::FunctionReference callback;
  PathWatcherWorker* worker;
  int watch_count;
  int id;

#ifdef __APPLE__
  // macOS.
  int kqueue;
  int init_errno;
#endif

#ifdef __linux__
  // Linux.
  int inotify;
  int init_errno;
#endif
};
