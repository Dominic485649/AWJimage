#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/types.h>
#endif

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <exception>
#include <limits>

#ifdef _WIN32
import awj.update_windows;

int run_cli(int argc, wchar_t* argv[]);
int run_studio_ui(const wchar_t* health_event = nullptr,
                  const wchar_t* installed_version = nullptr);
#else
import awj.update_linux;

int run_cli(int argc, char* argv[]);
int run_studio_ui();
#endif
int run_shell_convert_window(int argc, wchar_t* argv[]);
int run_shell_context_menu_helper(int argc, wchar_t* argv[]);

#ifdef _WIN32
namespace {

void enable_process_dpi_awareness() noexcept {
  const HMODULE user32 = GetModuleHandleW(L"user32.dll");
  if (user32 == nullptr) {
    return;
  }
  using SetDpiAwarenessContext = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
  auto set_dpi_awareness_context = reinterpret_cast<SetDpiAwarenessContext>(
      GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
  if (set_dpi_awareness_context != nullptr) {
    set_dpi_awareness_context(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  }
}

void release_explorer_console_if_lonely() {
  std::array<DWORD, 2> process_ids{};
  const DWORD count = GetConsoleProcessList(
      process_ids.data(), static_cast<DWORD>(process_ids.size()));
  if (count <= 1) {
    FreeConsole();
  }
}

// ---------------------------------------------------------------------------
// 崩溃留痕
//
// GUI 进程从资源管理器启动时没有控制台：一旦异常逃到 std::terminate，UCRT 会
// 直接 abort()，Windows 记下 0xC0000409 / FAST_FAIL_FATAL_APP_EXIT(7) 就没有别的
// 线索了。这里的目标不是"接住"崩溃——terminate 之后已经无法安全恢复——而是在
// 死掉之前留下一行可读的原因，把"启动一会自己就没了"变成可定位的问题。
//
// 约束：处理函数在堆和 CRT 状态都可能已经不可靠时运行，所以里面只用 Win32 原语，
// 不分配内存、不用 std::format、不碰 iostream。日志路径在进程还健康的时候
// (install_crash_diagnostics) 预先算好存进静态缓冲区。
// ---------------------------------------------------------------------------

constexpr std::size_t crash_path_capacity = MAX_PATH + 32;

// 预先算好的两个候选路径：先写 exe 同目录（和 AWJ.jsonc 配置同一位置，用户第一
// 眼会去那里找），失败再退到 %LOCALAPPDATA%（Program Files 下 exe 目录不可写）。
wchar_t crash_log_primary[crash_path_capacity]{};
wchar_t crash_log_fallback[crash_path_capacity]{};

// 当前阶段，供崩溃时区分是 Studio 界面还是右键转换窗口出的问题。
std::atomic<const char*> crash_stage{"startup"};

void append_wide(wchar_t* buffer, std::size_t capacity, const wchar_t* text) noexcept {
  std::size_t used = 0;
  while (used < capacity && buffer[used] != L'\0') {
    ++used;
  }
  for (std::size_t i = 0; text[i] != L'\0' && used + 1 < capacity; ++i) {
    buffer[used++] = text[i];
  }
  if (used < capacity) {
    buffer[used] = L'\0';
  }
}

// 只用 WriteFile 追加一行，失败就换下一个候选路径，全失败就安静放弃。
bool append_crash_line(const wchar_t* path, const char* text) noexcept {
  if (path[0] == L'\0') {
    return false;
  }
  const HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ,
                                  nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                  nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return false;
  }
  std::size_t length = 0;
  while (text[length] != '\0') {
    ++length;
  }
  DWORD written = 0;
  const BOOL ok = WriteFile(file, text, static_cast<DWORD>(length), &written,
                            nullptr);
  CloseHandle(file);
  return ok != FALSE;
}

// 把一个 64 位值按十六进制写进 buffer，不依赖 CRT 格式化。
void append_hex(char* buffer, std::size_t capacity, unsigned long long value) noexcept {
  constexpr char digits[] = "0123456789abcdef";
  char scratch[17]{};
  int count = 0;
  do {
    scratch[count++] = digits[value & 0xFull];
    value >>= 4;
  } while (value != 0 && count < 16);

  std::size_t used = 0;
  while (used < capacity && buffer[used] != '\0') {
    ++used;
  }
  while (count > 0 && used + 1 < capacity) {
    buffer[used++] = scratch[--count];
  }
  if (used < capacity) {
    buffer[used] = '\0';
  }
}

void append_ascii(char* buffer, std::size_t capacity, const char* text) noexcept {
  std::size_t used = 0;
  while (used < capacity && buffer[used] != '\0') {
    ++used;
  }
  for (std::size_t i = 0; text[i] != '\0' && used + 1 < capacity; ++i) {
    buffer[used++] = text[i];
  }
  if (used < capacity) {
    buffer[used] = '\0';
  }
}

void record_crash(const char* kind, const char* detail,
                  unsigned long long code) noexcept {
  char line[512]{};
  append_ascii(line, sizeof(line), "AWJ crash: ");
  append_ascii(line, sizeof(line), kind);
  append_ascii(line, sizeof(line), " stage=");
  const char* stage = crash_stage.load(std::memory_order_relaxed);
  append_ascii(line, sizeof(line), stage != nullptr ? stage : "unknown");
  if (code != 0) {
    append_ascii(line, sizeof(line), " code=0x");
    append_hex(line, sizeof(line), code);
  }
  if (detail != nullptr && detail[0] != '\0') {
    append_ascii(line, sizeof(line), " detail=");
    append_ascii(line, sizeof(line), detail);
  }
  append_ascii(line, sizeof(line), "\r\n");

  if (!append_crash_line(crash_log_primary, line)) {
    append_crash_line(crash_log_fallback, line);
  }
}

// std::terminate 处理函数。走到这里说明有异常没人接，或者违反了 noexcept。
// 先把还活着的异常对象里的 what() 抠出来，再交回 abort() —— 保持原有行为，
// Windows 错误报告仍会照常生成 minidump，不影响既有的排查手段。
[[noreturn]] void on_terminate() noexcept {
  try {
    if (auto current = std::current_exception()) {
      std::rethrow_exception(current);
    }
    record_crash("terminate", "no active exception (likely a noexcept violation)", 0);
  } catch (const std::exception& error) {
    record_crash("terminate", error.what(), 0);
  } catch (...) {
    record_crash("terminate", "non-std exception", 0);
  }
  std::abort();
}

// SEH 兜底：访问违例这类结构化异常不会经过 terminate。记完一行就把控制权交回
// 系统（EXCEPTION_CONTINUE_SEARCH），让 WER 继续产生 minidump。
LONG WINAPI on_unhandled_exception(EXCEPTION_POINTERS* info) noexcept {
  unsigned long long code = 0;
  if (info != nullptr && info->ExceptionRecord != nullptr) {
    code = info->ExceptionRecord->ExceptionCode;
  }
  record_crash("unhandled-seh", "", code);
  return EXCEPTION_CONTINUE_SEARCH;
}

void install_crash_diagnostics() noexcept {
  // exe 同目录。GetModuleFileNameW 失败就留空，append_crash_line 会跳过它。
  wchar_t module_path[MAX_PATH]{};
  const DWORD length =
      GetModuleFileNameW(nullptr, module_path, static_cast<DWORD>(MAX_PATH));
  if (length > 0 && length < MAX_PATH) {
    std::size_t last_separator = 0;
    for (std::size_t i = 0; module_path[i] != L'\0'; ++i) {
      if (module_path[i] == L'\\' || module_path[i] == L'/') {
        last_separator = i;
      }
    }
    module_path[last_separator] = L'\0';
    append_wide(crash_log_primary, crash_path_capacity, module_path);
    append_wide(crash_log_primary, crash_path_capacity, L"\\AWJ-crash.log");
  }

  wchar_t local_appdata[MAX_PATH]{};
  const DWORD appdata_length = GetEnvironmentVariableW(
      L"LOCALAPPDATA", local_appdata, static_cast<DWORD>(MAX_PATH));
  if (appdata_length > 0 && appdata_length < MAX_PATH) {
    append_wide(crash_log_fallback, crash_path_capacity, local_appdata);
    append_wide(crash_log_fallback, crash_path_capacity, L"\\AWJ-crash.log");
  }

  std::set_terminate(&on_terminate);
  SetUnhandledExceptionFilter(&on_unhandled_exception);
}

// 两条 GUI 出口的最外层护栏。异常逃到这里已经无法继续，但把它变成一条崩溃记录
// 加一个明确的退出码，总比静默 abort 好。
template <class Entry>
int run_guarded(const char* stage, Entry&& entry) noexcept {
  crash_stage.store(stage, std::memory_order_relaxed);
  try {
    return entry();
  } catch (const std::exception& error) {
    record_crash("escaped-exception", error.what(), 0);
  } catch (...) {
    record_crash("escaped-exception", "non-std exception", 0);
  }
  return 3;
}

}  // namespace
#endif

#ifdef _WIN32
int wmain(int argc, wchar_t* argv[]) {
  // 最先装崩溃留痕：后面任何一步失败都还能留下一行原因。
  install_crash_diagnostics();
  const auto parse_pid = [](const wchar_t* text) -> DWORD {
    if (text == nullptr || *text == L'\0') return 0;
    wchar_t* end = nullptr;
    const unsigned long value = std::wcstoul(text, &end, 10);
    return end != nullptr && *end == L'\0' && value != 0 &&
                   value <= std::numeric_limits<DWORD>::max()
               ? static_cast<DWORD>(value)
               : 0;
  };
  if (argc == 3 && std::wcscmp(argv[1], L"--update-helper") == 0) {
    const DWORD parent_pid = parse_pid(argv[2]);
    return parent_pid == 0 ? 19 : awj::update::run_update_helper(parent_pid);
  }
  if (argc == 3 && std::wcscmp(argv[1], L"--update-recover") == 0) {
    const DWORD parent_pid = parse_pid(argv[2]);
    return parent_pid == 0 ? 19
                           : awj::update::run_update_recovery_helper(parent_pid);
  }
  if (argc == 4 && std::wcscmp(argv[1], L"--update-health-check") == 0) {
    enable_process_dpi_awareness();
    release_explorer_console_if_lonely();
    return run_guarded("update-health-check", [argv] {
      return run_studio_ui(argv[2], argv[3]);
    });
  }
  if (argc == 2 &&
      (std::wcscmp(argv[1], L"--shell-context-menu-helper") == 0 ||
       std::wcscmp(argv[1], L"--shell-context-menu-remove-helper") == 0)) {
    return run_guarded("shell-context-menu-helper", [argc, argv] {
      return run_shell_context_menu_helper(argc, argv);
    });
  }
  if (awj::update::launch_update_recovery_if_needed()) {
    return 0;
  }
  enable_process_dpi_awareness();
  if (argc <= 1) {
    release_explorer_console_if_lonely();
    return run_guarded("studio-ui", [] { return run_studio_ui(nullptr, nullptr); });
  }

  for (int i = 1; i < argc; ++i) {
    if (std::wcscmp(argv[i], L"--shell-window") == 0) {
      release_explorer_console_if_lonely();
      return run_guarded("shell-window",
                         [argc, argv] { return run_shell_convert_window(argc, argv); });
    }
  }

  crash_stage.store("cli", std::memory_order_relaxed);
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::setvbuf(stderr, nullptr, _IONBF, 0);
  const int exit_code = run_cli(argc, argv);
  std::fflush(stdout);
  std::fflush(stderr);
  // 部分第三方 native codec 的静态运行时会在进程静态析构阶段
  // 触发清理顺序问题。CLI 到这里时工作已完成，直接交还退出码；
  // std::_Exit 避免运行 CRT/第三方静态析构，比 ExitProcess 更稳定。
  std::_Exit(exit_code);
}
#else
int main(int argc, char* argv[]) {
  const auto parse_pid = [](const char* text) -> pid_t {
    if (text == nullptr || *text == '\0') return 0;
    char* end = nullptr;
    const auto value = std::strtol(text, &end, 10);
    return end != nullptr && *end == '\0' && value > 0 &&
                   value <= std::numeric_limits<pid_t>::max()
               ? static_cast<pid_t>(value)
               : 0;
  };
  if (argc == 3 && std::strcmp(argv[1], "--linux-update-helper") == 0) {
    const auto pid = parse_pid(argv[2]);
    return pid == 0 ? 19 : awj::update::run_linux_update_helper(pid);
  }
  if (argc == 2 && std::strcmp(argv[1], "--linux-update-health-check") == 0) {
    return 0;
  }
  if (awj::update::launch_linux_update_recovery_if_needed()) {
    return 0;
  }
  if (argc <= 1) {
    return run_studio_ui();
  }
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::setvbuf(stderr, nullptr, _IONBF, 0);
  const int exit_code = run_cli(argc, argv);
  std::fflush(stdout);
  std::fflush(stderr);
  return exit_code;
}
#endif
