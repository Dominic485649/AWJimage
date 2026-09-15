#include "shell_elevation.hpp"

#define NOMINMAX
#include <windows.h>
#include <ktmw32.h>
#include <objbase.h>
#include <sddl.h>
#include <shellapi.h>
#include <shlobj_core.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <limits>
#include <stdexcept>
#include <utility>

namespace awj::shell_context_menu {
namespace {

struct Handle {
  HANDLE value{};
  explicit Handle(HANDLE h = nullptr) : value(h) {}
  ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
};

struct LocalMemory {
  void* value{};
  ~LocalMemory() { if (value) LocalFree(value); }
};

struct WireFormat {
  std::array<std::array<wchar_t, 32>, 8> text{};
  std::array<std::int32_t, 13> number{};
};
struct Request {
  std::uint64_t magic{0x41574a4d454e5514ull};
  std::uint64_t transaction{};
  std::uint32_t remove{};
  std::array<WireFormat, 5> formats{};
};
struct Reply {
  DWORD error{};
  std::array<char, 1024> message{};
};
static_assert(sizeof(Request) < 8192);

[[noreturn]] void system_error(const char* operation) {
  throw std::runtime_error(std::format("{} failed (Windows {}).", operation, GetLastError()));
}

std::filesystem::path process_exe(HANDLE process) {
  std::wstring path(32768, L'\0');
  DWORD length = static_cast<DWORD>(path.size());
  if (!QueryFullProcessImageNameW(process, 0, path.data(), &length)) system_error("Query process image");
  path.resize(length);
  return std::filesystem::path{path};
}

bool same_executable(HANDLE process) {
  const auto current = process_exe(GetCurrentProcess());
  const auto peer = process_exe(process);
  std::error_code ec;
  return std::filesystem::equivalent(current, peer, ec) && !ec;
}

bool elevated() {
  HANDLE raw{};
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) return false;
  Handle token{raw};
  TOKEN_ELEVATION elevation{};
  DWORD size{};
  return GetTokenInformation(raw, TokenElevation, &elevation, sizeof(elevation), &size) &&
         elevation.TokenIsElevated;
}

std::wstring pipe_security() {
  HANDLE raw{};
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) system_error("Open caller token");
  Handle token{raw};
  DWORD size{};
  GetTokenInformation(raw, TokenUser, nullptr, 0, &size);
  std::vector<unsigned char> bytes(size);
  if (!GetTokenInformation(raw, TokenUser, bytes.data(), size, &size)) system_error("Read caller SID");
  wchar_t* sid{};
  if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid, &sid))
    system_error("Format caller SID");
  LocalMemory memory{sid};
  return L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;" + std::wstring{sid} + L")";
}

void finish_io(HANDLE pipe, OVERLAPPED& overlap, DWORD& transferred) {
  const auto waited = WaitForSingleObject(overlap.hEvent, 30000);
  if (waited != WAIT_OBJECT_0) {
    CancelIoEx(pipe, &overlap);
    GetOverlappedResult(pipe, &overlap, &transferred, TRUE);
    throw std::runtime_error("Menu helper IPC timed out; the transaction was not committed.");
  }
  if (!GetOverlappedResult(pipe, &overlap, &transferred, FALSE)) system_error("Complete menu IPC");
}

void transfer(HANDLE pipe, void* data, DWORD size, bool write) {
  Handle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
  if (!event.value) system_error("Create menu IPC event");
  OVERLAPPED overlap{};
  overlap.hEvent = event.value;
  DWORD transferred{};
  const BOOL ok = write ? WriteFile(pipe, data, size, &transferred, &overlap)
                        : ReadFile(pipe, data, size, &transferred, &overlap);
  if (!ok) {
    if (GetLastError() != ERROR_IO_PENDING) system_error("Transfer menu IPC");
    finish_io(pipe, overlap, transferred);
  }
  if (transferred != size) throw std::runtime_error("Invalid menu helper message size.");
}

WireFormat encode(const FormatParams& p) {
  WireFormat wire;
  const std::array texts{p.quality_text, p.bit_depth_text, p.speed_text,
      p.max_width_text, p.max_height_text, p.max_long_edge_text,
      p.max_short_edge_text, p.scale_percent_text};
  for (std::size_t i = 0; i < texts.size(); ++i) {
    if (texts[i].size() >= wire.text[i].size() ||
        std::ranges::any_of(texts[i], [](wchar_t c) { return c < L'0' || c > L'9'; }))
      throw std::runtime_error("Menu numeric parameters are invalid.");
    std::ranges::copy(texts[i], wire.text[i].begin());
  }
  wire.number = {p.avif_encoder_index, p.avif_color_representation_index,
      p.chroma_index, p.alpha_policy_index, p.jpegli_progressive_index,
      p.jpegli_optimize_huffman, p.jpegli_xyb, p.jxl_jpeg_lossless,
      p.strip_metadata, p.allow_wic_fallback, p.close_on_finish,
      p.install_avif_png_command, p.size_limit_index};
  return wire;
}

FormatParams decode(const WireFormat& wire) {
  const std::array maxima{1, 2, 3, 2, 2, 1, 1, 1, 1, 1, 1, 1, 2};
  for (std::size_t i = 0; i < maxima.size(); ++i)
    if (wire.number[i] < 0 || wire.number[i] > maxima[i])
      throw std::runtime_error("Menu helper rejected an invalid option.");
  std::array<std::wstring, 8> text;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const auto end = std::ranges::find(wire.text[i], L'\0');
    if (end == wire.text[i].end()) throw std::runtime_error("Unterminated menu parameter.");
    text[i].assign(wire.text[i].begin(), end);
    if (std::ranges::any_of(text[i], [](wchar_t c) { return c < L'0' || c > L'9'; }))
      throw std::runtime_error("Menu helper rejected nonnumeric text.");
  }
  return FormatParams{
      .quality_text = text[0], .bit_depth_text = text[1], .speed_text = text[2],
      .avif_encoder_index = wire.number[0], .avif_color_representation_index = wire.number[1],
      .chroma_index = wire.number[2], .alpha_policy_index = wire.number[3],
      .jpegli_progressive_index = wire.number[4], .jpegli_optimize_huffman = wire.number[5] != 0,
      .jpegli_xyb = wire.number[6] != 0, .jxl_jpeg_lossless = wire.number[7] != 0,
      .strip_metadata = wire.number[8] != 0, .allow_wic_fallback = wire.number[9] != 0,
      .close_on_finish = wire.number[10] != 0, .install_avif_png_command = wire.number[11] != 0,
      .size_limit_index = wire.number[12], .max_width_text = text[3], .max_height_text = text[4],
      .max_long_edge_text = text[5], .max_short_edge_text = text[6], .scale_percent_text = text[7]};
}

void elevated_stage(HANDLE transaction, const MenuParams& params, bool remove_menu) {
  GUID guid{};
  if (FAILED(CoCreateGuid(&guid))) system_error("Create menu session");
  wchar_t nonce[40]{};
  StringFromGUID2(guid, nonce, 40);
  const auto pid = GetCurrentProcessId();
  const auto name = std::format(L"\\\\.\\pipe\\AWJimage.Menu.{}.{}", pid, nonce);
  PSECURITY_DESCRIPTOR raw_security{};
  const auto sddl = pipe_security();
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
                                                            &raw_security, nullptr))
    system_error("Create menu pipe ACL");
  LocalMemory security{raw_security};
  SECURITY_ATTRIBUTES attributes{sizeof(attributes), raw_security, FALSE};
  Handle pipe{CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
      FILE_FLAG_FIRST_PIPE_INSTANCE, PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE |
      PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 16384, 16384, 30000, &attributes)};
  if (pipe.value == INVALID_HANDLE_VALUE) system_error("Create menu pipe");

  const auto exe = process_exe(GetCurrentProcess());
  const auto arguments = std::format(L"--shell-menu-helper {} {}", pid, nonce);
  const auto directory = exe.parent_path().wstring();
  SHELLEXECUTEINFOW launch{};
  launch.cbSize = sizeof(launch);
  launch.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
  launch.lpVerb = L"runas";
  launch.lpFile = exe.c_str();
  launch.lpParameters = arguments.c_str();
  launch.lpDirectory = directory.c_str();
  launch.nShow = SW_HIDE;
  if (!ShellExecuteExW(&launch)) {
    if (GetLastError() == ERROR_CANCELLED)
      throw std::runtime_error("用户取消了管理员权限请求，右键菜单未修改。");
    system_error("Start elevated menu helper");
  }
  Handle child{launch.hProcess};
  Handle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
  if (!event.value) system_error("Create connection event");
  OVERLAPPED overlap{};
  overlap.hEvent = event.value;
  if (!ConnectNamedPipe(pipe.value, &overlap)) {
    const auto error = GetLastError();
    if (error == ERROR_IO_PENDING) {
      DWORD ignored{};
      finish_io(pipe.value, overlap, ignored);
    } else if (error != ERROR_PIPE_CONNECTED) system_error("Connect menu helper");
  }
  ULONG client{};
  if (!GetNamedPipeClientProcessId(pipe.value, &client) || client != GetProcessId(child.value) ||
      !same_executable(child.value)) throw std::runtime_error("Unexpected menu helper process.");
  Request request;
  request.transaction = reinterpret_cast<std::uintptr_t>(transaction);
  request.remove = remove_menu;
  for (std::size_t i = 0; i < params.size(); ++i) request.formats[i] = encode(params[i]);
  transfer(pipe.value, &request, sizeof(request), true);
  Reply reply;
  transfer(pipe.value, &reply, sizeof(reply), false);
  if (reply.error) {
    reply.message.back() = '\0';
    throw std::runtime_error(reply.message.data());
  }
  if (WaitForSingleObject(child.value, 30000) != WAIT_OBJECT_0)
    throw std::runtime_error("Menu helper did not exit; transaction rolled back.");
  DWORD exit_code{};
  if (!GetExitCodeProcess(child.value, &exit_code) || exit_code)
    throw std::runtime_error("Menu helper failed; transaction rolled back.");
}

}  // namespace

MenuTransaction::~MenuTransaction() {
  if (handle_) {
    RollbackTransaction(handle_);
    CloseHandle(handle_);
  }
}

std::expected<void, std::string> MenuTransaction::commit() {
  if (!handle_ || !CommitTransaction(handle_))
    return std::unexpected{std::format("提交右键菜单事务失败：{}。", GetLastError())};
  CloseHandle(std::exchange(handle_, nullptr));
  SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
  return {};
}

std::expected<std::shared_ptr<MenuTransaction>, std::string> prepare_menu_change(
    const std::filesystem::path& exe, const MenuParams& params,
    std::span<const std::wstring> names, bool compatibility, bool remove_menu) {
  const auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  struct ComGuard { HRESULT result; ~ComGuard() { if (SUCCEEDED(result)) CoUninitialize(); } } apartment{com};
  try {
    if (auto restored = recover(); !restored) return std::unexpected{restored.error()};
    const auto previous = compatibility_installed();
    if (!previous) return std::unexpected{previous.error()};
    const auto machine = legacy_machine_commands();
    if (!machine) return std::unexpected{machine.error()};
    Handle raw{CreateTransaction(nullptr, nullptr, 0, 0, 0, 120000, nullptr)};
    if (raw.value == INVALID_HANDLE_VALUE) system_error("Create registry transaction");
    auto transaction = std::make_shared<MenuTransaction>(raw.value);
    raw.value = nullptr;
    if ((!remove_menu && compatibility) || *previous || !machine->empty()) {
      const bool remove_machine = remove_menu || !compatibility;
      if (elevated()) {
        if (auto staged = stage_machine_menu(transaction->handle(), exe, params, remove_machine); !staged)
          return std::unexpected{staged.error()};
      } else {
        elevated_stage(transaction->handle(), params, remove_machine);
      }
    }
    if (auto staged = stage_user_menu(transaction->handle(), exe, params, names,
                                      compatibility, remove_menu); !staged)
      return std::unexpected{staged.error()};
    return transaction;
  } catch (const std::exception& error) {
    return std::unexpected{error.what()};
  } catch (...) {
    return std::unexpected{"右键菜单事务失败，未提交修改。"};
  }
}

int run_elevation_helper(int argc, wchar_t* argv[]) noexcept {
  try {
    if (argc != 4 || !elevated()) return 2;
    std::uint64_t pid{};
    for (const wchar_t c : std::wstring_view{argv[2]}) {
      if (c < L'0' || c > L'9' || pid > MAXDWORD / 10) return 2;
      pid = pid * 10 + c - L'0';
    }
    if (!pid || pid > MAXDWORD) return 2;
    const std::wstring nonce{argv[3]};
    GUID guid{};
    if (nonce.size() != 38 || FAILED(CLSIDFromString(nonce.c_str(), &guid))) return 2;
    const auto name = std::format(L"\\\\.\\pipe\\AWJimage.Menu.{}.{}", pid, nonce);
    Handle pipe{CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                            OPEN_EXISTING, FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT |
                            SECURITY_IDENTIFICATION, nullptr)};
    if (pipe.value == INVALID_HANDLE_VALUE) return 3;
    ULONG server{};
    if (!GetNamedPipeServerProcessId(pipe.value, &server) || server != pid) return 4;
    Handle parent{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_DUP_HANDLE,
                              FALSE, server)};
    if (!parent.value || !same_executable(parent.value)) return 4;
    Request request;
    transfer(pipe.value, &request, sizeof(request), false);
    if (request.magic != Request{}.magic || request.remove > 1) return 5;
    Reply reply;
    try {
      HANDLE duplicated{};
      if (!DuplicateHandle(parent.value, reinterpret_cast<HANDLE>(request.transaction),
          GetCurrentProcess(), &duplicated, 0, FALSE, DUPLICATE_SAME_ACCESS))
        system_error("Duplicate caller transaction");
      Handle transaction{duplicated};
      DWORD outcome{}, isolation{}, flags{}, timeout{};
      if (!GetTransactionInformation(duplicated, &outcome, &isolation, &flags, &timeout, 0, nullptr))
        system_error("Validate caller transaction");
      MenuParams params;
      for (std::size_t i = 0; i < params.size(); ++i) params[i] = decode(request.formats[i]);
      const auto exe = process_exe(GetCurrentProcess());
      if (auto result = stage_machine_menu(duplicated, exe, params, request.remove != 0); !result)
        throw std::runtime_error(result.error());
    } catch (const std::exception& error) {
      reply.error = 1;
      const auto text = std::string_view{error.what()};
      std::copy_n(text.begin(), std::min(text.size(), reply.message.size() - 1), reply.message.begin());
    }
    transfer(pipe.value, &reply, sizeof(reply), true);
    return reply.error ? 1 : 0;
  } catch (...) {
    return 6;
  }
}

}  // namespace awj::shell_context_menu
