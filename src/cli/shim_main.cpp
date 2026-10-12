#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <vector>

int run_cli(int argc, wchar_t* argv[]);

int wmain(int argc, wchar_t* argv[]) {
  if (argc > 1) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    const int result = run_cli(argc, argv);
    std::fflush(stdout);
    std::fflush(stderr);
    // Match AWJ.exe: output is committed; avoid codec static teardown ordering.
    std::_Exit(result);
  }
  std::vector<wchar_t> path(32768);
  const DWORD length = GetModuleFileNameW(nullptr, path.data(),
                                         static_cast<DWORD>(path.size()));
  if (length == 0 || length >= path.size()) return 1;
  const auto gui = std::filesystem::path{path.data()}.replace_extension(L".exe");
  STARTUPINFOW startup{.cb = sizeof(STARTUPINFOW)};
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(gui.c_str(), nullptr, nullptr, nullptr, FALSE, 0,
                      nullptr, nullptr, &startup, &process)) {
    std::fprintf(stderr, "Cannot start AWJ.exe (Win32 error %lu).\n", GetLastError());
    return 1;
  }
  CloseHandle(process.hThread);
  const DWORD waited = WaitForSingleObject(process.hProcess, INFINITE);
  DWORD result = 1;
  if (waited == WAIT_OBJECT_0) GetExitCodeProcess(process.hProcess, &result);
  CloseHandle(process.hProcess);
  return static_cast<int>(result);
}
