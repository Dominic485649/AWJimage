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
#include <optional>
#include <random>
#include <span>
#include <cstddef>
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
#ifndef AWJ_MEMORY_CLI_ONLY
import awj.pipeline;
#endif
import awj.decoder_registry;
import awj.preset;
#ifdef _WIN32
import awj.image;
import awj.codec;
import awj.native_backend;
import awj.resource_planner;
import awj.encoding_defaults;
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

std::expected<std::size_t, std::string> read_stdin_bytes(std::span<std::byte> bytes, std::stop_token stop) {
  const auto input = GetStdHandle(STD_INPUT_HANDLE);
  if (!input || input == INVALID_HANDLE_VALUE) return std::unexpected{"stdin 句柄无效。"};
  const auto type = GetFileType(input);
  if (type != FILE_TYPE_PIPE && type != FILE_TYPE_DISK) return std::unexpected{"WGC stdin 必须来自二进制管道或文件。"};
  while (!stop.stop_requested()) {
    DWORD available = static_cast<DWORD>(std::min<std::size_t>(bytes.size(), 65536));
    if (type == FILE_TYPE_PIPE) {
      if (!PeekNamedPipe(input, nullptr, 0, nullptr, &available, nullptr)) {
        if (GetLastError() == ERROR_BROKEN_PIPE) return 0;
        return std::unexpected{"查询 WGC stdin 管道失败。"};
      }
      if (!available) { std::this_thread::sleep_for(std::chrono::milliseconds{2}); continue; }
      available = static_cast<DWORD>(std::min<std::size_t>(available, std::min<std::size_t>(bytes.size(), 65536)));
    }
    DWORD received{};
    if (!ReadFile(input, bytes.data(), available, &received, nullptr)) {
      if (GetLastError() == ERROR_BROKEN_PIPE) return 0;
      return std::unexpected{"读取 WGC stdin 失败。"};
    }
    return static_cast<std::size_t>(received);
  }
  return std::unexpected{"任务已取消。"};
}

std::expected<awj::ImageDecodeResult, std::string> read_wgc_stdin(
    const WgcStdinSpec& spec, const awj::AppConfig& config, std::stop_token stop) {
  const auto stride = static_cast<std::uint64_t>(spec.width) * 8;
  const auto maximum = awj::encoding_defaults::effective_max_input_file_bytes();
  if (stride > maximum || spec.height > maximum / stride ||
      (config.memory_limit_bytes && spec.height > config.memory_limit_bytes / stride))
    return std::unexpected{"WGC stdin 帧超过输入或配置内存上限。"};
  awj::ImageDecodeResult decoded;
  decoded.decoder_id = "stdin-wgc-memory";
  auto& image = decoded.image;
  image.width = spec.width; image.height = spec.height;
  image.pixel_format = awj::PixelFormat::rgba; image.bit_depth = 16;
  image.alpha_mode = awj::AlphaMode::straight;
  image.sample_representation = awj::SampleRepresentation::ieee_half_float;
  image.source_info = awj::ImageSourceInfo{.pixel_format = awj::PixelFormat::rgba, .bit_depth = 16,
      .color_primaries = 1, .transfer_characteristics = 8, .matrix_coefficients = 0, .color_range = 1,
      .has_hdr_metadata = true, .color_metadata_source = "wgc-scrgb-half-linear", .linear_reference_white_nits = 80};
  image.planes.resize(1);
  auto& plane = image.planes[0]; plane.stride = static_cast<std::size_t>(stride);
  plane.bytes.resize(static_cast<std::size_t>(stride * spec.height));
  std::size_t offset{};
  while (offset < plane.bytes.size()) {
    auto received = read_stdin_bytes(std::span{plane.bytes}.subspan(offset), stop);
    if (!received) return std::unexpected{received.error()};
    if (!*received) return std::unexpected{"WGC stdin 数据短于声明的单帧。"};
    offset += *received;
  }
  std::byte extra{};
  auto tail = read_stdin_bytes({&extra, 1}, stop);
  if (!tail) return std::unexpected{tail.error()};
  if (*tail) return std::unexpected{"WGC stdin 数据长于声明的单帧；多帧请使用 awj-stream。"};
  return decoded;
}

std::expected<int, std::string> run_wgc_memory(const WgcStdinSpec& spec, awj::AppConfig config, std::stop_token stop) {
  if (config.output_dir.empty()) return std::unexpected{"--stdin-wgc-rgba16f 必须显式指定 -o/--output。"};
  if (config.output_template.find(L"{sha256") != std::wstring::npos)
    return std::unexpected{"内存 stdin 不提供原输入文件的 SHA-256 模板；请使用帧号或普通名称。"};
  auto decoded = read_wgc_stdin(spec, config, stop);
  if (!decoded) return std::unexpected{decoded.error()};
  const auto input_bytes = decoded->image.planes[0].bytes.size();
  std::uint64_t hash = 14695981039346656037ull;
  for (const auto byte : decoded->image.planes[0].read_bytes()) { hash ^= std::to_integer<unsigned char>(byte); hash *= 1099511628211ull; }
  std::mt19937_64 random{static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count())};
  config.input_path = L"stdin-wgc-rgba16f.awsraw"; // 仅作为最终文件命名提示，不创建/打开此文件。
  config.write_log = false; config.write_summary = false;
  awj::FileLogger logger{{}, false};
  const auto resources = awj::plan_resources({.automatic_thread_budget = std::max(1, config.max_jobs),
      .file_count = 1, .memory_limit_bytes = config.memory_limit_bytes, .estimated_bytes_per_file = input_bytes});
  awj::NativeBackend backend{config, logger, resources};
  auto encoded = backend.encode_memory(std::move(*decoded), stop);
  if (!encoded) return std::unexpected{encoded.error()};
  const auto source = awj::make_image_file(0, config.input_path, {}, input_bytes, random, std::format(L"{:016x}", hash));
  auto output = awj::resolve_collision_output_path(awj::output_path_for(config, source), config.collision_mode, nullptr, awj::output_extension_for(config));
  if (!output) return std::unexpected{output.error()};
  std::error_code error;
  if (config.collision_mode == awj::CollisionMode::skip && std::filesystem::exists(*output, error) && !error) return 0;
  auto written = awj::native_backend_detail::write_output_bytes(*output, encoded->encoded.bytes,
      config.collision_mode == awj::CollisionMode::overwrite, stop);
  if (!written) return std::unexpected{written.error()};
  std::println("[OK] {}", awj::display_path_for_user(*output));
  return 0;
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
    print_line("[INFO] 未找到有效用户预设。预设目录位于程序同目录 preset/。");
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
#endif

  try {
    if (args.empty()) {
      awj::print_help();
      return 0;
    }
    if (!args.empty() && args.front() == L"--studio-probe-image") {
      if (args.size() != 3 || (args[2] != L"wic" && args[2] != L"native")) {
        print_line("图片尺寸探测参数无效。");
        return 1;
      }
      auto dimensions = awj::probe_image_dimensions_for_path(
          std::filesystem::path{args[1]}, awj::DecoderRegistryOptions{.allow_wic_fallback = args[2] == L"wic"});
      if (!dimensions) { print_line(dimensions.error()); return 1; }
      print_line(std::format("{} {}", dimensions->width, dimensions->height));
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
    if (auto affinity = awj::apply_cpu_affinity(parsed->config.cpu_affinity); !affinity) {
      print_line(std::format("[FAIL] {}", affinity.error()));
      return 1;
    }
    if (!parsed->config.cpu_affinity.empty()) {
      print_line(std::format("[INFO] CPU affinity: {} logical CPUs; thread budget {}.",
          parsed->config.cpu_affinity.size(), parsed->config.max_jobs));
    }
#ifndef _WIN32
    std::jthread signal_watcher{[](std::stop_token token) {
      while (!token.stop_requested()) {
        if (g_cli_signal_seen != 0) {
          if (!g_cli_stop_requested.exchange(true)) g_cli_stop_source.request_stop();
          return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
      }
    }};
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
    const auto exit_code = capture_expected<int>([&]() -> std::expected<int, std::string> {
      const auto token = g_cli_stop_source.get_token();
#ifdef _WIN32
      if (*wgc_stdin) return run_wgc_memory(**wgc_stdin, parsed->config, token);
#endif
#ifdef AWJ_MEMORY_CLI_ONLY
      return std::unexpected{"此无 GUI 验证目标仅接受 --stdin-wgc-rgba16f。"};
#else
      if (parsed->config.output_policy == awj::OutputPolicy::shell &&
          !parsed->shell_inputs.empty()) {
        return awj::run_pipeline(parsed->config, parsed->shell_inputs, token);
      }
      return awj::run_pipeline(parsed->config, token);
#endif
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
    args.emplace_back(awj::argument_text_from_native(argv[i] != nullptr ? std::string_view{argv[i]} : std::string_view{}));
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
