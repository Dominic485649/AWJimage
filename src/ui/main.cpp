#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifdef _WIN32

// Windows Studio 的入口薄壳。全部装配逻辑已拆到 studio_run_windows.cpp。

#include "studio_run_windows.h"

int run_studio_ui(const wchar_t* health_event,
                  const wchar_t* installed_version) {
  return awj::studio::run_studio_ui(health_event, installed_version);
}

int run_shell_convert_window(int argc, wchar_t* argv[]) {
  return awj::studio::run_shell_convert_window(argc, argv);
}

#else

// Linux Studio 的入口薄壳。全部实现已拆到 studio_run_linux.cpp。

#include "studio_run_linux.h"

int run_studio_ui() { return awj::studio::run_studio_ui(); }

#endif
