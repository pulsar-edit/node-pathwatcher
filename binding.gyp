{
  "targets": [
    {
      "target_name": "efsw",
      "type": "static_library",
      "sources": [
        "./vendor/efsw/src/efsw/Debug.cpp",
        "./vendor/efsw/src/efsw/DirWatcherGeneric.cpp",
        "./vendor/efsw/src/efsw/DirectorySnapshot.cpp",
        "./vendor/efsw/src/efsw/DirectorySnapshotDiff.cpp",
        "./vendor/efsw/src/efsw/FileInfo.cpp",
        "./vendor/efsw/src/efsw/FileSystem.cpp",
        "./vendor/efsw/src/efsw/FileWatcher.cpp",
        "./vendor/efsw/src/efsw/FileWatcherCWrapper.cpp",
        "./vendor/efsw/src/efsw/FileWatcherFSEvents.cpp",
        "./vendor/efsw/src/efsw/FileWatcherGeneric.cpp",
        "./vendor/efsw/src/efsw/FileWatcherImpl.cpp",
        "./vendor/efsw/src/efsw/FileWatcherInotify.cpp",
        "./vendor/efsw/src/efsw/FileWatcherKqueue.cpp",
        "./vendor/efsw/src/efsw/FileWatcherWin32.cpp",
        "./vendor/efsw/src/efsw/Log.cpp",
        "./vendor/efsw/src/efsw/Mutex.cpp",
        "./vendor/efsw/src/efsw/String.cpp",
        "./vendor/efsw/src/efsw/System.cpp",
        "./vendor/efsw/src/efsw/Thread.cpp",
        "./vendor/efsw/src/efsw/Watcher.cpp",
        "./vendor/efsw/src/efsw/WatcherFSEvents.cpp",
        "./vendor/efsw/src/efsw/WatcherGeneric.cpp",
        "./vendor/efsw/src/efsw/WatcherInotify.cpp",
        "./vendor/efsw/src/efsw/WatcherKqueue.cpp",
        "./vendor/efsw/src/efsw/WatcherWin32.cpp"
      ],
      "include_dirs": [
        "./vendor/efsw/include",
        "./vendor/efsw/src"
      ],
      "conditions": [
        ["OS==\"win\"", {
          "sources!": [
            "./vendor/efsw/src/efsw/WatcherKqueue.cpp",
            "./vendor/efsw/src/efsw/WatcherFSEvents.cpp",
            "./vendor/efsw/src/efsw/WatcherInotify.cpp",
            "./vendor/efsw/src/efsw/FileWatcherKqueue.cpp",
            "./vendor/efsw/src/efsw/FileWatcherInotify.cpp",
            "./vendor/efsw/src/efsw/FileWatcherFSEvents.cpp"
          ],
          "sources": [
            "./vendor/efsw/src/efsw/platform/win/FileSystemImpl.cpp",
            "./vendor/efsw/src/efsw/platform/win/MutexImpl.cpp",
            "./vendor/efsw/src/efsw/platform/win/SystemImpl.cpp",
            "./vendor/efsw/src/efsw/platform/win/ThreadImpl.cpp"
          ],
        }],
        ["OS!=\"win\"", {
          "sources": [
            "./vendor/efsw/src/efsw/platform/posix/FileSystemImpl.cpp",
            "./vendor/efsw/src/efsw/platform/posix/MutexImpl.cpp",
            "./vendor/efsw/src/efsw/platform/posix/SystemImpl.cpp",
            "./vendor/efsw/src/efsw/platform/posix/ThreadImpl.cpp"
          ],
          "cflags": ["-Wall", "-Wno-long-long"]
        }],
        ["OS==\"linux\"", {
          "sources!": [
            "./vendor/efsw/src/efsw/WatcherKqueue.cpp",
            "./vendor/efsw/src/efsw/WatcherFSEvents.cpp",
            "./vendor/efsw/src/efsw/WatcherWin32.cpp",
            "./vendor/efsw/src/efsw/FileWatcherKqueue.cpp",
            "./vendor/efsw/src/efsw/FileWatcherWin32.cpp",
            "./vendor/efsw/src/efsw/FileWatcherFSEvents.cpp"
          ],
          "libraries": [
            "-lpthread"
          ],
          "defines": [
            "EFSW_VERBOSE"
          ]
        }],
        ["OS==\"mac\"", {
          "sources!": [
            "./vendor/efsw/src/efsw/WatcherInotify.cpp",
            "./vendor/efsw/src/efsw/WatcherWin32.cpp",
            "./vendor/efsw/src/efsw/FileWatcherInotify.cpp",
            "./vendor/efsw/src/efsw/FileWatcherWin32.cpp"
          ],
          "defines": [
            "EFSW_FSEVENTS_SUPPORTED"
          ],
          "xcode_settings": {
            "OTHER_LDFLAGS": [
              "-framework CoreFoundation -framework CoreServices"
            ]
          }
        }]
      ]
    },
    {
      "target_name": "pathwatcher",
      "defines": [
        "NODE_API_SWALLOW_UNTHROWABLE_EXCEPTIONS"
      ],
      "dependencies": ["efsw"],
      "cflags!": ["-fno-exceptions"],
      "cflags_cc!": ["-fno-exceptions"],
      "xcode_settings": {
        "GCC_ENABLE_CPP_EXCEPTIONS": "YES",
        "CLANG_CXX_LIBRARY": "libc++",
        "MACOSX_DEPLOYMENT_TARGET": "10.7",
      },
      "msvs_settings": {
        "VCCLCompilerTool": {"ExceptionHandling": 1},
      },
      "sources": [
        "lib/core.cc",
        "lib/core.h"
      ],
      "include_dirs": [
        "<!(node -p \"require('node-addon-api').include_dir\")",
        "vendor/efsw",
      ],
      "conditions": [
        ['OS=="mac"', {
          "sources+": [
            "lib/platform/FSEventsFileWatcher.cpp"
          ]
        }],
        ['OS=="win"', {
          'msvs_settings': {
            'VCCLCompilerTool': {
              'ExceptionHandling': 1,  # /EHsc
              'WarnAsError': 'true',
            },
          },
          'msvs_disabled_warnings': [
            4018,  # signed/unsigned mismatch
            4244,  # conversion from 'type1' to 'type2', possible loss of data
            4267,  # conversion from 'size_t' to 'type', possible loss of data
            4530,  # C++ exception handler used, but unwind semantics are not
            # enabled
            4506,  # no definition for inline function
            4577,  # 'noexcept' used with no exception handling mode specified;
            # termination on exception is not guaranteed
            4996,  # function was declared deprecated
            2220,  # warning treated as error - no object file generated
            4309,  # 'conversion' : truncation of constant value
            4101,  # unreferenced local variable
          ],
          'defines': [
            '_WIN32_WINNT=0x0600',
          ],
        }]  # OS=="win"
      ],
    }
  ]
}
