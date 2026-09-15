#include "studio_shell_cli.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cwctype>
#include <format>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>

#include "studio_fields.h"
#include "studio_ui_util.h"
#include "studio_worker_control.h"

import awj.core;
import awj.encoding_defaults;
import awj.preset;
import awj.studio_defaults;

namespace awj::studio {

std::wstring queue_path_key(const std::filesystem::path& path) {
  std::error_code ec;
  const auto absolute = std::filesystem::absolute(path, ec);
  return awj::normalized_lower_path_key(ec ? path : absolute);
}


std::wstring cli_output_format_arg(awj::OutputFormat format) {
  switch (format) {
    case awj::OutputFormat::png:
      return L"png";
    case awj::OutputFormat::webp:
      return L"webp";
    case awj::OutputFormat::jxl:
      return L"jxl";
    case awj::OutputFormat::jpgli:
      return L"jpgli";
    case awj::OutputFormat::avif:
    default:
      return L"avif";
  }
}

std::wstring cli_collision_arg(awj::CollisionMode mode) {
  switch (mode) {
    case awj::CollisionMode::skip:
      return L"skip";
    case awj::CollisionMode::suffix_time:
      return L"time";
    case awj::CollisionMode::suffix_random:
      return L"random";
    case awj::CollisionMode::suffix_number:
      return L"number";
    case awj::CollisionMode::overwrite:
    default:
      return L"overwrite";
  }
}

std::wstring cli_chroma_arg(awj::ChromaMode mode) {
  return awj::wide_from_utf8(awj::chroma_mode_name(mode));
}

std::wstring cli_avif_encoder_arg(awj::AvifEncoderMode mode) {
  return awj::wide_from_utf8(awj::avif_encoder_mode_name(mode));
}

std::wstring cli_alpha_arg(awj::AlphaModePolicy policy) {
  return awj::wide_from_utf8(awj::alpha_mode_policy_name(policy));
}

void push_cli_option(std::vector<std::wstring>& args, std::wstring option,
                     std::wstring value) {
  args.push_back(std::move(option));
  args.push_back(std::move(value));
}

void push_cli_option(std::vector<std::wstring>& args, std::wstring option,
                     const std::filesystem::path& value) {
  push_cli_option(args, std::move(option), value.native());
}

std::wstring bytes_argument(std::uint64_t bytes) {
  return bytes == 0 ? std::wstring{L"auto"} : std::format(L"{}b", bytes);
}

std::vector<std::wstring> cli_arguments_from_config(
    const awj::AppConfig& cfg, std::wstring_view cancel_event_name) {
  std::vector<std::wstring> args;
  args.reserve(80);

  push_cli_option(args, L"--input", cfg.input_path);
  if (!cfg.output_dir.empty()) {
    push_cli_option(args, L"--output", cfg.output_dir);
  }
  push_cli_option(args, L"--format", cli_output_format_arg(cfg.output_format));
  push_cli_option(args, L"--template", cfg.output_template);
  push_cli_option(args, L"--threads", std::to_wstring(cfg.max_jobs));
  push_cli_option(args, L"--memory-limit", bytes_argument(cfg.memory_limit_bytes));
  switch (cfg.image_size_limit.mode) {
    case awj::ImageSizeLimitMode::none:
      push_cli_option(args, L"--image-size-limit", std::wstring{L"none"});
      break;
    case awj::ImageSizeLimitMode::manual:
      push_cli_option(args, L"--image-size-limit", std::wstring{L"manual"});
      if (cfg.image_size_limit.max_width) push_cli_option(args, L"--max-width", std::to_wstring(*cfg.image_size_limit.max_width));
      if (cfg.image_size_limit.max_height) push_cli_option(args, L"--max-height", std::to_wstring(*cfg.image_size_limit.max_height));
      if (cfg.image_size_limit.max_long_edge) push_cli_option(args, L"--max-long-edge", std::to_wstring(*cfg.image_size_limit.max_long_edge));
      if (cfg.image_size_limit.max_short_edge) push_cli_option(args, L"--max-short-edge", std::to_wstring(*cfg.image_size_limit.max_short_edge));
      if (cfg.image_size_limit.scale_percent) push_cli_option(args, L"--scale-percent", std::to_wstring(*cfg.image_size_limit.scale_percent));
      break;
    case awj::ImageSizeLimitMode::automatic:
    default:
      push_cli_option(args, L"--image-size-limit", std::wstring{L"auto"});
      break;
  }
  push_cli_option(args, L"--timeout-encode",
                  std::to_wstring(cfg.encode_timeout_minutes));
  push_cli_option(args, L"--collision", cli_collision_arg(cfg.collision_mode));
  push_cli_option(args, L"--studio-cancel-event",
                  std::wstring{cancel_event_name});
  if (!cfg.studio_queue_manifest.empty()) {
    push_cli_option(args, L"--studio-queue-manifest",
                    cfg.studio_queue_manifest);
  }
  if (!cfg.studio_large_action.empty()) {
    push_cli_option(args, L"--studio-large-action", cfg.studio_large_action);
  }
  if (cfg.unlock_max_input_file_bytes) {
    args.push_back(L"--unlock-max-input-file-bytes");
  }

  if (cfg.visual_quality) {
    push_cli_option(args, L"--visual-quality",
                    std::to_wstring(*cfg.visual_quality));
    args.push_back(cfg.visual_quality_fallback ? L"--visual-quality-fallback"
                                               : L"--no-visual-quality-fallback");
    args.push_back(cfg.visual_quality_gpu ? L"--visual-quality-gpu"
                                          : L"--no-visual-quality-gpu");
  } else {
    push_cli_option(args, L"--quality", std::to_wstring(cfg.quality));
  }

  if (cfg.bit_depth) {
    push_cli_option(args, L"--bit-depth", std::to_wstring(*cfg.bit_depth));
  }
  if (cfg.speed) {
    push_cli_option(args, L"--speed", std::to_wstring(*cfg.speed));
  }

  args.push_back(cfg.allow_wic_fallback ? L"--allow-wic-fallback"
                                        : L"--no-wic-fallback");
  args.push_back(cfg.experimental_clamped_grid_padding
                     ? L"--experimental-clamped-grid-padding"
                     : L"--no-experimental-clamped-grid-padding");
  args.push_back(cfg.strip_metadata ? L"--strip" : L"--keep-metadata");
  args.push_back(cfg.write_summary ? L"--summary" : L"--no-summary");
  args.push_back(cfg.write_log ? L"--log" : L"--no-log");
  if (cfg.preserve_creation_time) args.push_back(L"--preserve-creation-time");
  if (cfg.preserve_modification_time) args.push_back(L"--preserve-modification-time");
  if (cfg.preserve_access_time) args.push_back(L"--preserve-access-time");

  if (cfg.output_format == awj::OutputFormat::avif) {
    push_cli_option(args, L"--avif-encoder",
                    cli_avif_encoder_arg(cfg.avif_encoder));
    push_cli_option(args, L"--chroma", cli_chroma_arg(cfg.chroma_mode));
    push_cli_option(args, L"--alpha", cli_alpha_arg(cfg.alpha_policy));
  }
  if (cfg.output_format == awj::OutputFormat::jxl && !cfg.jxl_jpeg_lossless) {
    args.push_back(L"--no-jxl-jpeg-lossless");
  }
  if (cfg.output_format == awj::OutputFormat::jpgli) {
    push_cli_option(args, L"--chroma", cli_chroma_arg(cfg.chroma_mode));
    push_cli_option(args, L"--jpegli-progressive-level",
                    std::to_wstring(cfg.jpegli_progressive_level));
    args.push_back(cfg.jpegli_optimize_huffman
                       ? L"--jpegli-optimize-huffman"
                       : L"--no-jpegli-optimize-huffman");
    if (cfg.jpegli_xyb) {
      args.push_back(L"--jpegli-xyb");
    }
  }

  if (cfg.color_primaries) {
    push_cli_option(args, L"--color-primaries",
                    std::to_wstring(*cfg.color_primaries));
  }
  if (cfg.transfer_characteristics) {
    push_cli_option(args, L"--transfer-characteristics",
                    std::to_wstring(*cfg.transfer_characteristics));
  }
  if (cfg.matrix_coefficients) {
    push_cli_option(args, L"--matrix-coefficients",
                    std::to_wstring(*cfg.matrix_coefficients));
  }
  if (cfg.color_range) {
    push_cli_option(args, L"--color-range", std::to_wstring(*cfg.color_range));
  }



  return args;
}

std::wstring quote_windows_command_arg(std::wstring_view arg,
                                      bool force_quotes = false) {
  if (arg.empty()) {
    return L"\"\"";
  }
  const bool needs_quotes =
      force_quotes || arg.find_first_of(L" \t\n\v\"") != std::wstring_view::npos;
  if (!needs_quotes) {
    return std::wstring{arg};
  }
  std::wstring quoted;
  quoted.reserve(arg.size() + 2);
  quoted.push_back(L'\"');
  std::size_t backslashes = 0;
  for (const wchar_t ch : arg) {
    if (ch == L'\\') {
      ++backslashes;
      continue;
    }
    if (ch == L'\"') {
      quoted.append(backslashes * 2 + 1, L'\\');
      quoted.push_back(ch);
      backslashes = 0;
      continue;
    }
    quoted.append(backslashes, L'\\');
    backslashes = 0;
    quoted.push_back(ch);
  }
  quoted.append(backslashes * 2, L'\\');
  quoted.push_back(L'\"');
  return quoted;
}

std::wstring command_line_from_args(std::span<const std::wstring> args) {
  std::wstring command;
  bool first = true;
  for (const auto& arg : args) {
    if (!command.empty()) {
      command.push_back(L' ');
    }
    command += quote_windows_command_arg(arg, first);
    first = false;
  }
  return command;
}

std::expected<std::filesystem::path, std::string> awj_exe_path_for_shell_menu() {
  auto executable = awj::executable_path();
  if (!executable) {
    return std::unexpected{executable.error()};
  }
  auto path = *executable;
  auto extension = path.extension().wstring();
  std::ranges::transform(extension, extension.begin(),
                         [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
  if (extension == L".com") {
    auto exe = path;
    exe.replace_extension(L".exe");
    std::error_code ec;
    if (std::filesystem::exists(exe, ec) && !ec) {
      return exe;
    }
  }
  auto sibling = path.parent_path() / L"AWJ.exe";
  std::error_code ec;
  if (std::filesystem::exists(sibling, ec) && !ec) {
    return sibling;
  }
  return path;
}

awj::shell_context_menu::FormatParams shell_format_params(const MenuFormatParams& params) {
  const auto text = [](const std::string& value) {
    return awj::wide_from_utf8(trim_copy(value));
  };
  return awj::shell_context_menu::FormatParams{
      .quality_text = text(params.quality_text),
      .bit_depth_text = text(params.bit_depth_text),
      .speed_text = text(params.speed_text),
      .avif_encoder_index = params.avif_encoder_index,
      .avif_color_representation_index = params.avif_color_representation_index,
      .chroma_index = params.chroma_index,
      .alpha_policy_index = params.alpha_policy_index,
      .jpegli_progressive_index = params.jpegli_progressive_index,
      .jpegli_optimize_huffman = params.jpegli_optimize_huffman,
      .jpegli_xyb = params.jpegli_xyb,
      .jxl_jpeg_lossless = params.jxl_jpeg_lossless,
      .strip_metadata = params.strip_metadata,
      .allow_wic_fallback = params.allow_wic_fallback,
      .close_on_finish = params.close_on_finish,
      .install_avif_png_command = params.install_avif_png_command,
      .size_limit_index = params.size_limit_index,
      .max_width_text = text(params.max_width_text),
      .max_height_text = text(params.max_height_text),
      .max_long_edge_text = text(params.max_long_edge_text),
      .max_short_edge_text = text(params.max_short_edge_text),
      .scale_percent_text = text(params.scale_percent_text)};
}

awj::shell_context_menu::MenuParams shell_menu_params(
    const std::array<MenuFormatParams, 5>& menu_params) {
  awj::shell_context_menu::MenuParams converted{};
  for (std::size_t i = 0; i < menu_params.size(); ++i) {
    converted[i] = shell_format_params(menu_params[i]);
  }
  return converted;
}

std::wstring shell_batch_name(awj::OutputFormat format, bool append_png_suffix) {
  return std::format(L"AWJimage.ShellBatch.{}{}",
                     cli_output_format_arg(format),
                     append_png_suffix ? L".png-suffix" : L"");
}

std::expected<bool, std::string> collect_shell_launch_inputs(
    awj::OutputFormat format, bool append_png_suffix,
    std::vector<std::filesystem::path>& inputs) {
  const auto suffix = shell_batch_name(format, append_png_suffix);
  const auto mutex_name = L"Local\\" + suffix;
  UniqueWin32Handle mutex{CreateMutexW(nullptr, TRUE, mutex_name.c_str())};
  if (!mutex) {
    return std::unexpected{std::format("创建右键队列锁失败，错误码 {}。",
                                       GetLastError())};
  }
  const bool leader = GetLastError() != ERROR_ALREADY_EXISTS;
  const auto send_mutex_name = mutex_name + L".Send";
  UniqueWin32Handle send_mutex{
      CreateMutexW(nullptr, FALSE, send_mutex_name.c_str())};
  if (!send_mutex) {
    const DWORD error = GetLastError();
    if (leader) ReleaseMutex(mutex.get());
    return std::unexpected{std::format("创建右键队列发送锁失败，错误码 {}。",
                                       error)};
  }
  const auto slot_name = L"\\\\.\\mailslot\\" + suffix;
  if (!leader) {
    std::vector<std::filesystem::path> unsent;
    unsent.reserve(inputs.size());
    for (int attempt = 0; attempt < 25; ++attempt) {
      const DWORD gate = WaitForSingleObject(send_mutex.get(), 1000);
      if (gate != WAIT_OBJECT_0 && gate != WAIT_ABANDONED) {
        return true;
      }
      const DWORD leader_state = WaitForSingleObject(mutex.get(), 0);
      if (leader_state != WAIT_TIMEOUT) {
        if (leader_state == WAIT_OBJECT_0 || leader_state == WAIT_ABANDONED) {
          ReleaseMutex(mutex.get());
        }
        ReleaseMutex(send_mutex.get());
        return true;
      }
      UniqueWin32Handle slot{adopt_win32_handle(
          CreateFileW(slot_name.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr))};
      if (slot) {
        for (const auto& input : inputs) {
          const auto value = input.wstring();
          if (value.empty() || value.size() > 32760) {
            unsent.push_back(input);
            continue;
          }
          const auto byte_count =
              static_cast<DWORD>(value.size() * sizeof(wchar_t));
          DWORD written = 0;
          if (!WriteFile(slot.get(), value.data(), byte_count, &written,
                         nullptr) ||
              written != byte_count) {
            unsent.push_back(input);
          }
        }
        ReleaseMutex(send_mutex.get());
        if (unsent.empty()) return false;
        inputs = std::move(unsent);
        return true;
      }
      ReleaseMutex(send_mutex.get());
      Sleep(20);
    }
    return true;
  }

  const DWORD create_gate = WaitForSingleObject(send_mutex.get(), INFINITE);
  if (create_gate != WAIT_OBJECT_0 && create_gate != WAIT_ABANDONED) {
    ReleaseMutex(mutex.get());
    return std::unexpected{"无法锁定右键队列发送通道。"};
  }
  UniqueWin32Handle slot{
      adopt_win32_handle(CreateMailslotW(slot_name.c_str(), 65520, 0, nullptr))};
  if (!slot) {
    const DWORD error = GetLastError();
    ReleaseMutex(send_mutex.get());
    ReleaseMutex(mutex.get());
    return std::unexpected{std::format("创建右键队列通道失败，错误码 {}。",
                                       error)};
  }
  ReleaseMutex(send_mutex.get());
  std::unordered_set<std::wstring> seen;
  for (const auto& input : inputs) seen.insert(queue_path_key(input));
  const auto started = std::chrono::steady_clock::now();
  const auto hard_deadline = started + std::chrono::milliseconds{650};
  auto quiet_deadline = started + std::chrono::milliseconds{180};
  const auto drain_messages = [&] {
    DWORD next_size = MAILSLOT_NO_MESSAGE;
    DWORD message_count = 0;
    bool received_message = false;
    if (GetMailslotInfo(slot.get(), nullptr, &next_size, &message_count, nullptr) &&
        next_size != MAILSLOT_NO_MESSAGE) {
      while (message_count > 0 && next_size != MAILSLOT_NO_MESSAGE) {
        std::vector<wchar_t> buffer((next_size / sizeof(wchar_t)) + 1, L'\0');
        DWORD read = 0;
        if (ReadFile(slot.get(), buffer.data(), next_size, &read, nullptr) &&
            read > 0 && read % sizeof(wchar_t) == 0) {
          received_message = true;
          std::filesystem::path input{
              std::wstring{buffer.data(), read / sizeof(wchar_t)}};
          if (seen.insert(queue_path_key(input)).second) {
            inputs.push_back(std::move(input));
          }
        }
        if (!GetMailslotInfo(slot.get(), nullptr, &next_size, &message_count,
                             nullptr)) {
          break;
        }
      }
    }
    return received_message;
  };
  while (true) {
    const bool received_message = drain_messages();
    const auto now = std::chrono::steady_clock::now();
    if (received_message) {
      quiet_deadline = std::min(
          hard_deadline, now + std::chrono::milliseconds{80});
    }
    if (now >= hard_deadline || now >= quiet_deadline) break;
    Sleep(10);
  }

  // Stop new writers before the final drain so a successful late write cannot be lost.
  const DWORD close_gate = WaitForSingleObject(send_mutex.get(), INFINITE);
  if (close_gate != WAIT_OBJECT_0 && close_gate != WAIT_ABANDONED) {
    ReleaseMutex(mutex.get());
    return std::unexpected{"无法关闭右键队列发送通道。"};
  }
  (void)drain_messages();
  slot.reset();
  ReleaseMutex(mutex.get());
  ReleaseMutex(send_mutex.get());
  return true;
}

std::expected<void, std::string> synchronize_shell_context_menu(
    const std::array<MenuFormatParams, 5>& menu_params, bool force_install) {
  auto awj_exe = awj_exe_path_for_shell_menu();
  if (!awj_exe) return std::unexpected{awj_exe.error()};
  auto names = awj::injected_user_preset_names();
  if (!names) return std::unexpected{names.error()};
  auto compatibility = awj::shell_context_menu::compatibility_installed();
  if (!compatibility) return std::unexpected{compatibility.error()};
  return awj::shell_context_menu::reconcile(*awj_exe, shell_menu_params(menu_params), *names,
                                           force_install, *compatibility);
}

std::expected<void, std::string> remove_shell_context_menu() {
  return awj::shell_context_menu::remove();
}

std::optional<std::string> shell_context_menu_warning(
    const std::array<MenuFormatParams, 5>& menu_params) {
  auto awj_exe = awj_exe_path_for_shell_menu();
  if (!awj_exe) {
    return "无法检查右键菜单程序路径，请移除后重新安装。";
  }
  auto names = awj::injected_user_preset_names();
  if (!names) return names.error();
  auto compatibility = awj::shell_context_menu::compatibility_installed();
  if (!compatibility) return compatibility.error();
  if (*compatibility) {
    auto matches = awj::shell_context_menu::machine_menu_matches(*awj_exe, shell_menu_params(menu_params));
    if (!matches || !*matches) return "兼容性菜单需要修复；点击修复时需要管理员权限。";
  }
  auto checked = awj::shell_context_menu::warning(*awj_exe, shell_menu_params(menu_params), *names, *compatibility);
  if (!checked) {
    return "检查右键菜单注册表失败：" + checked.error();
  }
  return *checked;
}

std::expected<std::shared_ptr<StudioChildProcess>, std::string>
start_studio_cli_worker(const awj::AppConfig& cfg, std::uint64_t run_id) {
  auto exe_path = awj::executable_path();
  if (!exe_path) {
    return std::unexpected{exe_path.error()};
  }
  std::error_code ec;
  if (!std::filesystem::is_regular_file(*exe_path, ec) || ec) {
    return std::unexpected{std::format("找不到 Studio 编码 worker: {}。",
                                       awj::display_path_for_user(*exe_path))};
  }
  const auto exe_dir = exe_path->parent_path();

  auto child = std::make_shared<StudioChildProcess>();
  child->queue_manifest_path = cfg.studio_queue_manifest;
  child->temp_directories.push_back(awj::output_dir_for(cfg));
  const auto event_name = std::format(L"Local\\AWJStudioCancel-{}-{}",
                                      GetCurrentProcessId(), run_id);
  child->cancel_event.reset(CreateEventW(nullptr, TRUE, FALSE, event_name.c_str()));
  if (child->cancel_event == nullptr) {
    return std::unexpected{std::format("创建编码取消事件失败: {}",
                                       awj::win32_error_message(GetLastError()))};
  }

  auto args = cli_arguments_from_config(cfg, event_name);
  args.insert(args.begin(), exe_path->native());
  auto command_line = command_line_from_args(args);
  if (command_line.size() >= 32767) {
    return std::unexpected{"编码 worker 命令行超过 Windows 长度限制。"};
  }
  child->command_line = command_line;

  UniqueWin32Handle job{CreateJobObjectW(nullptr, nullptr)};
  if (job == nullptr) {
    return std::unexpected{std::format(
        "创建编码 worker Job Object 失败: {}",
        awj::win32_error_message(GetLastError()))};
  }
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation,
                               &limits, sizeof(limits))) {
    return std::unexpected{std::format(
        "配置编码 worker Job Object 失败: {}",
        awj::win32_error_message(GetLastError()))};
  }

  HANDLE pipe_read = nullptr;
  HANDLE pipe_write = nullptr;
  SECURITY_ATTRIBUTES pipe_security{.nLength = sizeof(SECURITY_ATTRIBUTES),
                                    .lpSecurityDescriptor = nullptr,
                                    .bInheritHandle = TRUE};
  if (!CreatePipe(&pipe_read, &pipe_write, &pipe_security, 0)) {
    return std::unexpected{std::format("创建编码 worker 输出管道失败: {}",
                                       awj::win32_error_message(GetLastError()))};
  }
  UniqueWin32Handle output_read{pipe_read};
  UniqueWin32Handle output_write{pipe_write};
  SetHandleInformation(output_read.get(), HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  startup.hStdOutput = output_write.get();
  startup.hStdError = output_write.get();
  PROCESS_INFORMATION process{};
  auto mutable_command_line = command_line;
  const auto cwd = exe_dir.native();
  const BOOL created = CreateProcessW(
      exe_path->c_str(), mutable_command_line.data(), nullptr, nullptr, TRUE,
      CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED,
      nullptr, cwd.c_str(),
      &startup, &process);
  if (!created) {
    return std::unexpected{std::format("启动编码 worker 失败: {}",
                                       awj::win32_error_message(GetLastError()))};
  }
  child->process.reset(process.hProcess);
  child->thread.reset(process.hThread);
  child->output_read = std::move(output_read);
  output_write.reset();
  child->process_id = process.dwProcessId;
  if (!AssignProcessToJobObject(job.get(), child->process.get())) {
    const DWORD error = GetLastError();
    TerminateProcess(child->process.get(),
                     awj::studio_defaults::worker_force_stop_exit_code);
    WaitForSingleObject(child->process.get(), INFINITE);
    return std::unexpected{std::format(
        "将编码 worker 加入 Job Object 失败: {}",
        awj::win32_error_message(error))};
  }
  child->job = std::move(job);
  if (ResumeThread(child->thread.get()) == static_cast<DWORD>(-1)) {
    const DWORD error = GetLastError();
    TerminateJobObject(child->job.get(),
                       awj::studio_defaults::worker_force_stop_exit_code);
    WaitForSingleObject(child->process.get(), INFINITE);
    return std::unexpected{std::format(
        "恢复编码 worker 执行失败: {}",
        awj::win32_error_message(error))};
  }
  return child;
}

void cleanup_studio_queue_manifest(
    const std::shared_ptr<StudioChildProcess>& child) noexcept {
  if (child == nullptr || child->queue_manifest_path.empty()) {
    return;
  }
  std::error_code ec;
  std::filesystem::remove(child->queue_manifest_path, ec);
  child->queue_manifest_path.clear();
}

void cleanup_forced_worker_temp_files(
    const std::shared_ptr<StudioChildProcess>& child) noexcept {
  try {
    if (child == nullptr || child->process_id == 0) {
      return;
    }
    const auto output_prefix =
        std::format(L".awj-output-{}-", child->process_id);
    const auto summary_prefix =
        std::format(L"summary.csv.tmp-{}-", child->process_id);
    for (const auto& directory : child->temp_directories) {
      std::error_code ec;
      std::filesystem::directory_iterator it{
          directory, std::filesystem::directory_options::skip_permission_denied,
          ec};
      const std::filesystem::directory_iterator end;
      while (!ec && it != end) {
        const auto entry = *it;
        it.increment(ec);
        const auto name = entry.path().filename().wstring();
        const bool output_temp =
            name.starts_with(output_prefix) && name.ends_with(L".tmp");
        const bool summary_temp = name.starts_with(summary_prefix);
        if (!output_temp && !summary_temp) {
          continue;
        }
        std::error_code type_ec;
        if (entry.is_regular_file(type_ec) && !type_ec) {
          std::error_code remove_ec;
          std::filesystem::remove(entry.path(), remove_ec);
        }
      }
    }
  } catch (...) {
  }
}

bool reject_when_worker_active(AwjStudio& app,
                               const std::shared_ptr<UiState>& state,
                               const char* message) {
  {
    std::scoped_lock lock{state->mutex};
    if (!state->worker_active && !state->menu_operation_active) {
      return false;
    }
  }
  app.set_status_text(to_shared(message));
  return true;
}

void trim_process_working_set() {
  SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1),
                           static_cast<SIZE_T>(-1));
}

LONG physical_extent_to_long(std::uint32_t value) noexcept {
  return static_cast<LONG>(std::min<std::uint32_t>(
      value, static_cast<std::uint32_t>(std::numeric_limits<LONG>::max())));
}

LONG add_window_extent(LONG origin, LONG extent) noexcept {
  if (origin > std::numeric_limits<LONG>::max() - extent) {
    return std::numeric_limits<LONG>::max();
  }
  return origin + extent;
}

void constrain_window_to_work_area(slint::Window& window) {
  auto physical_size = window.size();
  auto position = window.position();
  const LONG current_width = physical_extent_to_long(physical_size.width);
  const LONG current_height = physical_extent_to_long(physical_size.height);
  RECT current_rect{position.x, position.y,
                    add_window_extent(position.x, current_width),
                    add_window_extent(position.y, current_height)};
  LONG non_client_width = 0;
  LONG non_client_height = 0;
  if (const auto hwnd = window.win32_hwnd(); hwnd != nullptr) {
    RECT window_rect{};
    RECT client_rect{};
    if (GetWindowRect(hwnd, &window_rect) && GetClientRect(hwnd, &client_rect)) {
      current_rect = window_rect;
      non_client_width = std::max<LONG>(
          0, (window_rect.right - window_rect.left) -
                 (client_rect.right - client_rect.left));
      non_client_height = std::max<LONG>(
          0, (window_rect.bottom - window_rect.top) -
                 (client_rect.bottom - client_rect.top));
    }
  }

  HMONITOR monitor = MonitorFromRect(&current_rect, MONITOR_DEFAULTTONEAREST);
  MONITORINFO monitor_info{.cbSize = sizeof(MONITORINFO)};
  if (monitor == nullptr || !GetMonitorInfoW(monitor, &monitor_info)) {
    return;
  }

  const RECT& work = monitor_info.rcWork;
  const auto work_width = std::max<LONG>(1, work.right - work.left);
  const auto work_height = std::max<LONG>(1, work.bottom - work.top);
  const auto max_client_width = std::max<LONG>(1, work_width - non_client_width);
  const auto max_client_height =
      std::max<LONG>(1, work_height - non_client_height);
  const auto clamped_width = std::min<std::uint32_t>(
      physical_size.width, static_cast<std::uint32_t>(max_client_width));
  const auto clamped_height = std::min<std::uint32_t>(
      physical_size.height, static_cast<std::uint32_t>(max_client_height));

  if (clamped_width != physical_size.width ||
      clamped_height != physical_size.height) {
    physical_size = slint::PhysicalSize{{clamped_width, clamped_height}};
    window.set_size(physical_size);
  }

  const auto width =
      physical_extent_to_long(physical_size.width) + non_client_width;
  const auto height =
      physical_extent_to_long(physical_size.height) + non_client_height;
  const auto max_x = work.right - width;
  const auto max_y = work.bottom - height;
  const auto clamped_x =
      std::clamp<LONG>(current_rect.left, work.left,
                       std::max(work.left, max_x));
  const auto clamped_y =
      std::clamp<LONG>(current_rect.top, work.top,
                       std::max(work.top, max_y));
  if (clamped_x != current_rect.left || clamped_y != current_rect.top) {
    window.set_position(slint::PhysicalPosition{{clamped_x, clamped_y}});
  }
}

}  // namespace awj::studio
