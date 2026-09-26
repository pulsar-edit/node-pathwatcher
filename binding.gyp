{
  "targets": [
    {
      "target_name": "pathwatcher",
      "defines": [
        "NODE_API_SWALLOW_UNTHROWABLE_EXCEPTIONS"
      ],
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
        "lib/core.h",
        "lib/watcher.h"
      ],
      "include_dirs": [
        "<!(node -p \"require('node-addon-api').include_dir\")",
      ],
      "conditions": [
        ['OS=="linux"', {
          "sources+": [
            "lib/platform/InotifyFileWatcher.cpp"
          ],
        }],
        ['OS=="mac"', {
          "sources+": [
            "lib/platform/FSEventsFileWatcher.cpp",
            "lib/platform/KqueueFileWatcher.cpp"
          ],
          "defines+": [
            "USE_KQUEUE"
          ]
        }],
        ['OS=="win"', {
          "sources+": [
            "lib/platform/ReadDirectoryChangesFileWatcher.cpp"
          ],
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
