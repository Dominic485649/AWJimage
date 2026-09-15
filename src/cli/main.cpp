#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <print>
#include <cstdlib>
#include <cwctype>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <csignal>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#endif

import awj.config;
import awj.core;
import awj.pipeline;
import awj.preset;
#ifdef _WIN32
import awj.raw_image_io;
#endif

namespace {

std::stop_source g_cli_stop_source;
std::atomic_bool g_cli_stop_requested{false};
#ifndef _WIN32
volatile std::sig_atomic_t g_cli_signal_seen = 0;
#endif

#ifdef _WIN32
struct Win32HandleDeleter {
  using pointer = HANDLE;
  void operator()(HANDLE value) const noexcept {
    if (value != nullptr && value != INVALID_HANDLE_VALUE) {
      CloseHandle(value);
    }
  }
};

using UniqueWin32Handle = std::unique_ptr<void, Win32HandleDeleter>;

BOOL WINAPI console_control_handler(DWORD event) {
  switch (event) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
      if (g_cli_stop_requested.exchange(true)) {
        return FALSE;
      }
      g_cli_stop_source.request_stop();
      return TRUE;
    default:
      return FALSE;
  }
}

#else

void signal_handler(int) {
  g_cli_signal_seen = 1;
}

#endif

#ifdef _WIN32
struct WgcStdinSpec {
  std::uint32_t width{};
  std::uint32_t height{};
};

std::expected<std::uint32_t, std::string> parse_wgc_dimension(
    std::wstring_view value) {
  if (value.empty() || value.size() > 10) {
    return std::unexpected{"WGC 尺寸必须是正整数。"};
  }
  std::uint64_t parsed = 0;
  for (const wchar_t ch : value) {
    if (ch < L'0' || ch > L'9' ||
        parsed > (std::numeric_limits<std::uint32_t>::max() -
                  static_cast<std::uint32_t>(ch - L'0')) /
                     10u) {
      return std::unexpected{"WGC 尺寸必须是 1 到 4294967295 的整数。"};
    }
    parsed = parsed * 10u + static_cast<std::uint32_t>(ch - L'0');
  }
  if (parsed == 0) return std::unexpected{"WGC 尺寸不能为零。"};
  return static_cast<std::uint32_t>(parsed);
}

std::expected<std::optional<WgcStdinSpec>, std::string> extract_wgc_stdin_spec(
    std::vector<std::wstring>& args) {
  std::vector<std::wstring> filtered;
  filtered.reserve(args.size());
  std::optional<WgcStdinSpec> result;
  bool explicit_input = false;
  for (std::size_t index = 0; index < args.size(); ++index) {
    auto option = args[index];
    std::ranges::transform(option, option.begin(), [](wchar_t ch) {
      return static_cast<wchar_t>(std::towlower(ch));
    });
    if (option == L"-i" || option == L"--input") explicit_input = true;
    if (option != L"--stdin-wgc-rgba16f") {
      filtered.push_back(std::move(args[index]));
      continue;
    }
    if (result) return std::unexpected{"--stdin-wgc-rgba16f 只能出现一次。"};
    if (++index >= args.size()) {
      return std::unexpected{"--stdin-wgc-rgba16f 需要 <宽>x<高>。"};
    }
    const auto separator = args[index].find_first_of(L"xX");
    if (separator == std::wstring::npos ||
        args[index].find_first_of(L"xX", separator + 1) != std::wstring::npos) {
      return std::unexpected{"--stdin-wgc-rgba16f 尺寸应为 <宽>x<高>。"};
    }
    auto width = parse_wgc_dimension(std::wstring_view{args[index]}.substr(0, separator));
    auto height = parse_wgc_dimension(std::wstring_view{args[index]}.substr(separator + 1));
    if (!width || !height) {
      return std::unexpected{!width ? width.error() : height.error()};
    }
    result = WgcStdinSpec{.width = *width, .height = *height};
  }
  if (result && explicit_input) {
    return std::unexpected{"--stdin-wgc-rgba16f 不能与 -i/--input 同时使用。"};
  }
  args = std::move(filtered);
  return result;
}

struct WgcStdinTemporary {
  std::filesystem::path directory{};
  std::filesystem::path path{};
  bool owns_directory{};
  ~WgcStdinTemporary() {
    std::error_code ignored;
    if (owns_directory && !directory.empty()) {
      std::filesystem::remove_all(directory, ignored);
    }
  }
};

std::expected<std::unique_ptr<WgcStdinTemporary>, std::string>
spool_wgc_stdin(const WgcStdinSpec& spec) {
  std::error_code ec;
  const auto temp_root = std::filesystem::temp_directory_path(ec);
  if (ec) return std::unexpected{"无法定位 WGC stdin 临时目录。"};
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  auto temporary = std::make_unique<WgcStdinTemporary>();
  for (unsigned attempt = 0; attempt < 32; ++attempt) {
    temporary->directory = temp_root / std::format(
        L"AWJ-wgc-{}-{}-{}", GetCurrentProcessId(), stamp, attempt);
    if (std::filesystem::create_directory(temporary->directory, ec)) {
      temporary->owns_directory = true;
      break;
    }
    if (ec && ec != std::errc::file_exists) {
      return std::unexpected{"无法创建 WGC stdin 临时目录。"};
    }
    ec.clear();
  }
  if (!temporary->owns_directory || temporary->directory.empty() ||
      !std::filesystem::is_directory(temporary->directory, ec) || ec) {
    return std::unexpected{"无法创建 WGC stdin 临时目录。"};
  }
  temporary->path = temporary->directory / L"stdin-wgc-rgba16f.awsraw";
  if (_setmode(_fileno(stdin), _O_BINARY) == -1) {
    return std::unexpected{"无法将 WGC stdin 切换为二进制模式。"};
  }
  if (auto written = awj::write_wgc_scrgb_half_stream_file(
          temporary->path, spec.width, spec.height, std::cin);
      !written) {
    return std::unexpected{written.error()};
  }
  return temporary;
}
#endif

}  // namespace

void print_line(std::string_view text) {
  std::println("{}", text);
}

int list_user_presets_for_cli() {
  const auto catalog = awj::list_user_presets();
  if (!catalog) {
    print_line(std::format("[FAIL] {}", catalog.error()));
    return 1;
  }
  if (catalog->presets.empty()) {
    if (auto directory = awj::user_preset_directory()) {
      print_line(std::format("[INFO] 未找到有效用户预设。预设目录：{}。",
                             awj::path_to_utf8(*directory)));
    } else {
      print_line("[INFO] 未找到有效用户预设。未能定位预设目录。");
    }
  } else {
    print_line("[INFO] 可用用户预设：");
    for (const auto& preset : catalog->presets) {
      print_line(std::format("  {}{}", preset.name,
                             preset.description.empty()
                                 ? ""
                                 : std::format(" — {}", preset.description)));
    }
  }
  for (const auto& error : catalog->errors) {
    print_line(std::format("[WARN] {}", error));
  }
  return 0;
}

template <class Value, class Function>
std::expected<Value, std::string> capture_expected(Function&& fn) noexcept {
  try {
    return std::invoke(std::forward<Function>(fn));
  } catch (const std::exception&) {
    return std::unexpected{std::string{}};
  } catch (...) {
    return std::unexpected{std::string{}};
  }
}

int run_cli_args(std::vector<std::wstring> args) {
  g_cli_stop_source = std::stop_source{};
  g_cli_stop_requested.store(false, std::memory_order_release);
#ifndef _WIN32
  g_cli_signal_seen = 0;
#endif
#ifdef _WIN32
  // Windows 控制台默认代码页不是 UTF-8，先切换再输出中文日志。
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);
  if (!SetConsoleCtrlHandler(console_control_handler, TRUE)) {
    print_line("[WARN] Ctrl+C 取消处理注册失败，继续运行。");
  }
#else
  std::signal(SIGINT, signal_handler);
  std::jthread signal_watcher{[](std::stop_token token) {
    while (!token.stop_requested()) {
      if (g_cli_signal_seen != 0) {
        if (!g_cli_stop_requested.exchange(true)) {
          g_cli_stop_source.request_stop();
        }
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
  }};
#endif

  try {
    if (args.empty()) {
      awj::print_help();
      return 0;
    }
    if (args.size() == 1 && args.front() == L"--list-presets") {
      return list_user_presets_for_cli();
    }

#ifdef _WIN32
    auto wgc_stdin = extract_wgc_stdin_spec(args);
    if (!wgc_stdin) {
      print_line(std::format("[FAIL] {}", wgc_stdin.error()));
      return 1;
    }
#endif

    auto parsed = awj::parse_arguments_with_user_preset(args);
    if (!parsed) {
      print_line(std::format("[FAIL] {}", parsed.error()));
      return 1;
    }
    for (const auto& warning : parsed->warnings) {
      print_line(std::format("[WARN] {}", warning));
    }
    if (parsed->should_exit) {
      return parsed->exit_code;
    }
#ifdef _WIN32
    std::unique_ptr<WgcStdinTemporary> wgc_temporary;
    if (*wgc_stdin) {
      if (parsed->config.output_dir.empty()) {
        print_line("[FAIL] --stdin-wgc-rgba16f 必须显式指定 -o/--output，避免把结果写入临时目录。");
        return 1;
      }
      auto spooled = spool_wgc_stdin(**wgc_stdin);
      if (!spooled) {
        print_line(std::format("[FAIL] {}", spooled.error()));
        return 1;
      }
      parsed->config.input_path = (*spooled)->path;
      parsed->shell_inputs.clear();
      wgc_temporary = std::move(*spooled);
    }
#endif
    if (auto valid = awj::validate_execution_config(parsed->config); !valid) {
      print_line(std::format("[FAIL] {}", valid.error()));
      return 1;
    }
    std::jthread studio_cancel_watcher;
#ifdef _WIN32
    UniqueWin32Handle studio_cancel_event;
    if (!parsed->config.studio_cancel_event_name.empty()) {
      studio_cancel_event.reset(OpenEventW(SYNCHRONIZE, FALSE,
                                           parsed->config.studio_cancel_event_name.c_str()));
      if (studio_cancel_event != nullptr) {
        studio_cancel_watcher = std::jthread{[](std::stop_token token, HANDLE event_handle) {
          while (!token.stop_requested()) {
            const DWORD wait = WaitForSingleObject(event_handle, 100);
            if (wait == WAIT_OBJECT_0) {
              g_cli_stop_requested.store(true, std::memory_order_release);
              g_cli_stop_source.request_stop();
              return;
            }
            if (wait != WAIT_TIMEOUT) {
              return;
            }
          }
        }, studio_cancel_event.get()};
      }
    }
#endif
    const auto exit_code = capture_expected<int>([&] {
      const auto token = g_cli_stop_source.get_token();
      if (parsed->config.output_policy == awj::OutputPolicy::shell &&
          !parsed->shell_inputs.empty()) {
        return awj::run_pipeline(parsed->config, parsed->shell_inputs, token);
      }
      return awj::run_pipeline(parsed->config, token);
    });
    studio_cancel_watcher.request_stop();
    studio_cancel_watcher = {};
#ifdef _WIN32
    studio_cancel_event.reset();
#endif
    if (!exit_code) {
      if (exit_code.error().empty()) {
        print_line("[FAIL] 运行时异常，程序已安全退出。");
      } else {
        print_line(std::format("[FAIL] {}", exit_code.error()));
      }
      return 1;
    }
    return *exit_code;
  } catch (const std::exception&) {
    print_line("[FAIL] 运行时异常，程序已安全退出。");
    return 1;
  } catch (...) {
    print_line("[FAIL] 未知异常，程序已安全退出。");
    return 1;
  }
}

#ifdef _WIN32
int run_cli(int argc, wchar_t* argv[]) {
  std::vector<std::wstring> args;
  args.reserve(static_cast<std::size_t>(std::max(argc - 1, 0)));
  for (int i = 1; i < argc; ++i) {
    args.emplace_back(argv[i]);
  }
  return run_cli_args(std::move(args));
}
#else
int run_cli(int argc, char* argv[]) {
  std::vector<std::wstring> args;
  args.reserve(static_cast<std::size_t>(std::max(argc - 1, 0)));
  for (int i = 1; i < argc; ++i) {
    args.emplace_back(awj::wide_from_utf8(argv[i] != nullptr ? std::string_view{argv[i]} : std::string_view{}));
  }
  return run_cli_args(std::move(args));
}
#endif

#ifndef AWJ_UNIFIED_EXE
#ifdef _WIN32
int wmain(int argc, wchar_t* argv[]) {
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
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::setvbuf(stderr, nullptr, _IONBF, 0);
  const int exit_code = run_cli(argc, argv);
  std::fflush(stdout);
  std::fflush(stderr);
  return exit_code;
}
#endif
#endif
