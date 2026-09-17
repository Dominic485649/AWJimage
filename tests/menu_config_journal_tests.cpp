#define NOMINMAX
#include <windows.h>
#include "isolated_registry.hpp"
#include "menu_config_journal.hpp"
#include "shell_elevation.hpp"
#include "menu_transaction_state.hpp"
#include <cstdio>
#include <stdexcept>

namespace menu = awj::shell_context_menu;
namespace {
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void require(const std::expected<void, std::string>& result) {
  if (!result) throw std::runtime_error(result.error());
}
std::filesystem::path own_exe() {
  wchar_t path[32768]{};
  check(GetModuleFileNameW(nullptr, path, 32768) != 0, "read process path failed");
  return path;
}
}

int wmain(int argc, wchar_t** argv) try {
  const auto exe = own_exe();
  const std::wstring committed_id = L"{02B1DC4C-FAF4-46E9-A5E0-FE7DBD677211}";
  const std::wstring crashed_id = L"{02B1DC4C-FAF4-46E9-A5E0-FE7DBD677212}";
  const std::string previous = "{\n  \"shell_menu_compatibility\": false\n}\n";
  if (argc == 3 && std::wstring_view{argv[1]} == L"--crash") {
    check(std::wstring_view{argv[2]}.starts_with(L"Software\\AWJimage.Tests\\"), "invalid sandbox");
    HKEY sandbox{};
    check(RegOpenKeyExW(HKEY_CURRENT_USER, argv[2], 0, KEY_ALL_ACCESS, &sandbox) == ERROR_SUCCESS,
          "open child sandbox failed");
    check(RegOverridePredefKey(HKEY_CURRENT_USER, sandbox) == ERROR_SUCCESS, "override child HKCU failed");
    require(menu::begin_menu_config_journal(crashed_id, false, exe, previous));
    ExitProcess(0); // Deliberately skip C++ cleanup, leaving a recovery record.
  }
  check(argc == 1, "unexpected arguments");
  IsolatedRegistry sandbox;
  bool restored = false;
  const auto restore = [&](const std::optional<std::string>& content) -> std::expected<void, std::string> {
    check(content && *content == previous, "recovery changed previous config bytes");
    restored = true;
    return {};
  };
  {
    require(menu::begin_menu_config_journal(committed_id, false, exe, previous));
    check(!menu::recover_menu_config_journal(exe, restore), "recovered a live operation");
    check(!menu::begin_menu_config_journal(crashed_id, false, exe, previous), "concurrent journal accepted");
    require(menu::record_menu_commit(committed_id, false));
  }
  check(!menu::recover_menu_config_journal(exe, restore), "committed live journal was discarded by another instance");
  require(menu::discard_menu_config_journal());
  check(!restored, "committed transaction restored stale config");
  auto command = L"\"" + exe.wstring() + L"\" --crash \"" + sandbox.path + L"\"";
  STARTUPINFOW startup{sizeof(startup)};
  PROCESS_INFORMATION child{};
  check(CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child), "start crash child failed");
  const auto waited = WaitForSingleObject(child.hProcess, 10000);
  DWORD exit_code{};
  GetExitCodeProcess(child.hProcess, &exit_code);
  if (waited != WAIT_OBJECT_0) TerminateProcess(child.hProcess, 1);
  CloseHandle(child.hThread);
  CloseHandle(child.hProcess);
  check(waited == WAIT_OBJECT_0 && exit_code == 0, "crash child failed");
  check(!menu::recover_menu_config_journal(exe,
      [](const std::optional<std::string>&) -> std::expected<void, std::string> {
        return std::unexpected{"injected config write failure"};
      }), "config write failure was reported as recovered");
  require(menu::recover_menu_config_journal(exe, restore));
  check(restored, "crashed transaction did not restore config");
  restored = false;
  require(menu::recover_menu_config_journal(exe, restore));
  check(!restored, "recovery was not idempotent");
  std::puts("Menu config commit, live-owner exclusion, process crash and recovery passed.");
  return 0;
} catch (const std::exception& error) {
  std::fprintf(stderr, "%s\n", error.what());
  return 1;
}
