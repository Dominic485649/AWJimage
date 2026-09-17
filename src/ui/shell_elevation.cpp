#include "shell_elevation.hpp"
#include "menu_transaction_state.hpp"

#define NOMINMAX
#include <windows.h>
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
struct MenuTransaction::Impl {
  MenuOperationLock operation;
  HANDLE pipe{}, child{};
  std::wstring id;
  bool machine{}, user_staged{}, committed{};
  ~Impl() {
    if (pipe && pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe);
    if (child) CloseHandle(child);
  }
};
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
    std::wstring_view text{texts[i]};
    const auto first = text.find_first_not_of(L" \t\r\n");
    text = first == text.npos ? std::wstring_view{} :
        text.substr(first, text.find_last_not_of(L" \t\r\n") - first + 1);
    if (text.size() >= wire.text[i].size() ||
        std::ranges::any_of(text, [i](wchar_t c) {
          return (c < L'0' || c > L'9') && !(i == 0 && (c == L'.' || c == L'q' || c == L'Q'));
        }))
      throw std::runtime_error("Menu numeric parameters are invalid.");
    std::ranges::copy(text, wire.text[i].begin());
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
    if (std::ranges::any_of(text[i], [i](wchar_t c) {
          return (c < L'0' || c > L'9') && !(i == 0 && (c == L'.' || c == L'q' || c == L'Q'));
        }))
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

void elevated_stage(MenuTransaction::Impl& session, const MenuParams& params, bool remove_menu) {
  const auto& nonce = session.id;
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
  request.remove = remove_menu;
  for (std::size_t i = 0; i < params.size(); ++i) request.formats[i] = encode(params[i]);
  transfer(pipe.value, &request, sizeof(request), true);
  Reply reply;
  transfer(pipe.value, &reply, sizeof(reply), false);
  if (reply.error) {
    reply.message.back() = '\0';
    throw std::runtime_error(reply.message.data());
  }
  session.pipe = std::exchange(pipe.value, nullptr);
  session.child = std::exchange(child.value, nullptr);
}

}  // namespace

MenuTransaction::MenuTransaction(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
MenuTransaction::~MenuTransaction() {
  // A failed cleanup leaves the durable journal for the next operation.
  try { if (impl_ && !impl_->committed) (void)rollback(); } catch (...) {}
}
std::wstring_view MenuTransaction::id() const noexcept { return impl_->id; }
bool MenuTransaction::machine() const noexcept { return impl_->machine; }

std::expected<void, std::string> MenuTransaction::commit() {
  try {
    if (impl_->machine) {
      DWORD decision = 1;
      transfer(impl_->pipe, &decision, sizeof(decision), true);
      Reply reply;
      transfer(impl_->pipe, &reply, sizeof(reply), false);
      if (reply.error) {
        reply.message.back() = '\0';
        throw std::runtime_error(reply.message.data());
      }
    } else if (auto recorded = record_menu_commit(impl_->id, false); !recorded)
      return recorded;
  } catch (const std::exception& error) {
    auto committed = menu_commit_recorded(impl_->id, impl_->machine);
    if (!committed || !*committed) return std::unexpected{error.what()};
  }
  impl_->committed = true;
  (void)finish_user_menu(impl_->id, true);
  if (impl_->child) WaitForSingleObject(impl_->child, 30000);
  SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
  return {};
}

std::expected<void, std::string> MenuTransaction::rollback() {
  std::string error;
  if (impl_->pipe) {
    try {
      DWORD decision = 0;
      transfer(impl_->pipe, &decision, sizeof(decision), true);
      Reply reply;
      transfer(impl_->pipe, &reply, sizeof(reply), false);
      if (reply.error) { reply.message.back() = '\0'; error = reply.message.data(); }
    } catch (const std::exception& failure) { error = failure.what(); }
    CloseHandle(std::exchange(impl_->pipe, nullptr));
    if (WaitForSingleObject(impl_->child, 30000) != WAIT_OBJECT_0)
      error += " 提权进程尚未完成恢复，下一次菜单操作将重试。";
  }
  if (impl_->user_staged) {
    if (auto restored = finish_user_menu(impl_->id, false); !restored) error += restored.error();
    impl_->user_staged = false;
  }
  impl_->committed = true;
  if (!error.empty()) return std::unexpected{error};
  return {};
}

std::expected<std::shared_ptr<MenuTransaction>, std::string> prepare_menu_change(
    const std::filesystem::path& exe, const MenuParams& params,
    std::span<const std::wstring> names, bool compatibility, bool remove_menu,
    const std::function<void()>& elevation_requested) {
  const auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  struct ComGuard { HRESULT result; ~ComGuard() { if (SUCCEEDED(result)) CoUninitialize(); } } apartment{com};
  try {
    auto session = std::make_unique<MenuTransaction::Impl>();
    if (!session->operation.held()) return std::unexpected{"另一进程正在修改右键菜单。"};
    if (auto restored = recover(); !restored) return std::unexpected{restored.error()};
    const auto previous = compatibility_installed();
    if (!previous) return std::unexpected{previous.error()};
    const auto machine = legacy_machine_commands();
    if (!machine) return std::unexpected{machine.error()};
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) system_error("Create menu session");
    wchar_t id[40]{};
    StringFromGUID2(guid, id, 40);
    session->id = id;
    session->machine = (!remove_menu && compatibility) || *previous || !machine->empty();
    if (session->machine) {
      const bool remove_machine = remove_menu || !compatibility;
      if (elevation_requested) elevation_requested();
      elevated_stage(*session, params, remove_machine);
    }
    auto* prepared = session.get();
    auto transaction = std::make_shared<MenuTransaction>(std::move(session));
    prepared->user_staged = true;
    if (auto staged = stage_user_menu(prepared->id, prepared->machine, exe, params, names,
                                      compatibility, remove_menu); !staged) {
      auto rollback = transaction->rollback();
      return std::unexpected{staged.error() + (rollback ? "" : " " + rollback.error())};
    }
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
    Handle parent{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                              FALSE, server)};
    if (!parent.value || !same_executable(parent.value)) return 4;
    auto sid = process_user_sid(parent.value);
    if (!sid) return 4;
    Handle registration{CreateMutexW(nullptr, FALSE, L"Global\\AWJimage.MachineMenu")};
    if (!registration.value) return 4;
    const auto locked = WaitForSingleObject(registration.value, 0);
    if (locked != WAIT_OBJECT_0 && locked != WAIT_ABANDONED) return 4;
    struct Unlock { HANDLE handle; ~Unlock() { ReleaseMutex(handle); } } unlock{registration.value};
    struct Recovery { ~Recovery() { try { (void)recover_machine_menu(); } catch (...) {} } } recovery;
    Request request;
    transfer(pipe.value, &request, sizeof(request), false);
    if (request.magic != Request{}.magic || request.remove > 1) return 5;
    Reply reply;
    MenuParams params;
    const auto exe = process_exe(GetCurrentProcess());
    try {
      for (std::size_t i = 0; i < params.size(); ++i) params[i] = decode(request.formats[i]);
      if (auto result = stage_machine_menu(exe, params, request.remove != 0, nonce, *sid); !result)
        throw std::runtime_error(result.error());
    } catch (const std::exception& error) {
      reply.error = 1;
      const auto text = std::string_view{error.what()};
      std::copy_n(text.begin(), std::min(text.size(), reply.message.size() - 1), reply.message.begin());
    }
    transfer(pipe.value, &reply, sizeof(reply), true);
    if (!reply.error) {
      DWORD decision{};
      transfer(pipe.value, &decision, sizeof(decision), false);
      auto result = decision == 1 ? commit_machine_menu(nonce, *sid, exe, params, request.remove != 0)
                                  : recover_machine_menu();
      if (!result) {
        reply.error = 1;
        std::copy_n(result.error().begin(), std::min(result.error().size(), reply.message.size() - 1), reply.message.begin());
      }
      transfer(pipe.value, &reply, sizeof(reply), true);
    }
    return reply.error ? 1 : 0;
  } catch (...) {
    return 6;
  }
}

}  // namespace awj::shell_context_menu
