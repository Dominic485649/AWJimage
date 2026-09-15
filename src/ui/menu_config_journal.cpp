#include "menu_config_journal.hpp"
#include "menu_transaction_state.hpp"
#define NOMINMAX
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <vector>

namespace awj::shell_context_menu {
namespace {
constexpr wchar_t journal[] = L"Software\\AWJimage\\MenuConfigTransaction";
constexpr DWORD maximum_size = 4 * 1024 * 1024;
struct Key {
  HKEY value{};
  ~Key() { if (value) RegCloseKey(value); }
};
struct Header {
  DWORD pid{};
  FILETIME created{};
  DWORD existed{};
};
struct JournalLock {
  HANDLE handle{CreateMutexW(nullptr, FALSE, L"Local\\AWJimage.MenuConfigTransaction")};
  bool held{};
  JournalLock() {
    if (handle) {
      const auto status = WaitForSingleObject(handle, 0);
      held = status == WAIT_OBJECT_0 || status == WAIT_ABANDONED;
    }
  }
  ~JournalLock() {
    if (held) ReleaseMutex(handle);
    if (handle) CloseHandle(handle);
  }
};
std::expected<void, std::string> failure() {
  return std::unexpected{"右键菜单配置事务不可用；请关闭其他 AWJ 窗口后重试。"};
}
}

std::expected<void, std::string> discard_menu_config_journal() {
  const auto status = RegDeleteTreeW(HKEY_CURRENT_USER, journal);
  if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) return failure();
  return {};
}

std::expected<void, std::string> begin_menu_config_journal(
    std::wstring_view id, bool machine, const std::filesystem::path& exe,
    const std::optional<std::string>& previous) {
  JournalLock lock;
  if (!lock.held) return failure();
  if (id.size() != 38 || (previous && previous->size() > maximum_size)) return failure();
  Key key;
  DWORD disposition{};
  if (RegCreateKeyExW(HKEY_CURRENT_USER, journal, 0, nullptr, 0, KEY_ALL_ACCESS,
                      nullptr, &key.value, &disposition) != ERROR_SUCCESS) return failure();
  if (disposition != REG_CREATED_NEW_KEY) return failure();
  Header header{.pid = GetCurrentProcessId(), .existed = previous.has_value()};
  FILETIME exited{}, kernel{}, user{};
  if (!GetProcessTimes(GetCurrentProcess(), &header.created, &exited, &kernel, &user)) {
    (void)discard_menu_config_journal();
    return failure();
  }
  const auto path = exe.wstring();
  const DWORD ready = 1;
  const DWORD machine_value = machine;
  const std::wstring id_value{id};
  const auto set = [&](const wchar_t* name, DWORD type, const void* data, DWORD bytes) {
    return RegSetValueExW(key.value, name, 0, type, static_cast<const BYTE*>(data), bytes) == ERROR_SUCCESS;
  };
  if (!set(L"Owner", REG_BINARY, &header, sizeof(header)) ||
      !set(L"Id", REG_SZ, id_value.c_str(), static_cast<DWORD>((id_value.size() + 1) * sizeof(wchar_t))) ||
      !set(L"Machine", REG_DWORD, &machine_value, sizeof(machine_value)) ||
      !set(L"Exe", REG_SZ, path.c_str(), static_cast<DWORD>((path.size() + 1) * sizeof(wchar_t))) ||
      !set(L"Previous", REG_BINARY, previous ? previous->data() : "",
           previous ? static_cast<DWORD>(previous->size()) : 0) ||
      !set(L"Ready", REG_DWORD, &ready, sizeof(ready)) ||
      RegFlushKey(key.value) != ERROR_SUCCESS) {
    (void)discard_menu_config_journal();
    return failure();
  }
  return {};
}

std::expected<void, std::string> recover_menu_config_journal(
    const std::filesystem::path& exe, const RestoreMenuConfig& restore) {
  JournalLock lock;
  if (!lock.held) return failure();
  Key key;
  const auto status = RegOpenKeyExW(HKEY_CURRENT_USER, journal, 0, KEY_READ, &key.value);
  if (status == ERROR_FILE_NOT_FOUND) return {};
  if (status != ERROR_SUCCESS) return failure();
  DWORD ready{}, ready_bytes = sizeof(ready);
  const auto ready_status = RegGetValueW(key.value, nullptr, L"Ready", RRF_RT_REG_DWORD,
                                        nullptr, &ready, &ready_bytes);
  if (ready_status == ERROR_FILE_NOT_FOUND) return discard_menu_config_journal();
  if (ready_status != ERROR_SUCCESS || ready != 1) return failure();
  wchar_t id[40]{};
  DWORD machine{}, bytes = sizeof(id);
  if (RegGetValueW(key.value, nullptr, L"Id", RRF_RT_REG_SZ, nullptr, id, &bytes) != ERROR_SUCCESS)
    return failure();
  bytes = sizeof(machine);
  if (RegGetValueW(key.value, nullptr, L"Machine", RRF_RT_REG_DWORD, nullptr, &machine, &bytes) != ERROR_SUCCESS || machine > 1)
    return failure();
  auto committed = menu_commit_recorded(id, machine != 0);
  if (!committed) return std::unexpected{committed.error()};
  if (*committed) return discard_menu_config_journal();
  Header header;
  bytes = sizeof(header);
  if (RegGetValueW(key.value, nullptr, L"Owner", RRF_RT_REG_BINARY, nullptr, &header, &bytes) != ERROR_SUCCESS ||
      bytes != sizeof(header) || header.existed > 1) return failure();
  const auto owner = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, header.pid);
  if (owner) {
    FILETIME created{}, exited{}, kernel{}, user{};
    const bool active = GetProcessTimes(owner, &created, &exited, &kernel, &user) &&
        CompareFileTime(&created, &header.created) == 0 && WaitForSingleObject(owner, 0) == WAIT_TIMEOUT;
    CloseHandle(owner);
    if (active) return failure();
  } else if (GetLastError() != ERROR_INVALID_PARAMETER) return failure();
  wchar_t path[32768]{};
  bytes = sizeof(path);
  if (RegGetValueW(key.value, nullptr, L"Exe", RRF_RT_REG_SZ, nullptr, path, &bytes) != ERROR_SUCCESS ||
      _wcsicmp(exe.c_str(), path) != 0) return failure();
  bytes = 0;
  if (RegGetValueW(key.value, nullptr, L"Previous", RRF_RT_REG_BINARY, nullptr, nullptr, &bytes) != ERROR_SUCCESS ||
      bytes > maximum_size) return failure();
  std::string previous(bytes, '\0');
  if (RegGetValueW(key.value, nullptr, L"Previous", RRF_RT_REG_BINARY, nullptr, previous.data(), &bytes) != ERROR_SUCCESS)
    return failure();
  if (auto result = restore(header.existed ? std::optional{previous} : std::nullopt); !result) return result;
  return discard_menu_config_journal();
}
}
