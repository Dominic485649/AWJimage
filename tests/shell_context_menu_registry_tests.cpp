#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "shell_context_menu.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

int fail(std::string_view message) {
  std::fwrite(message.data(), 1, message.size(), stderr);
  std::fputc('\n', stderr);
  return 1;
}

bool missing(LSTATUS status) {
  return status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND;
}

bool key_exists(std::wstring_view key) {
  HKEY raw = nullptr;
  const std::wstring path{key};
  const auto status = RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_READ, &raw);
  if (status == ERROR_SUCCESS) {
    RegCloseKey(raw);
    return true;
  }
  return false;
}

bool seed_legacy_key() {
  constexpr std::wstring_view paths[] = {
      L"Software\\Classes\\AWJImage.ContextMenu\\shell\\legacy-probe",
      L"Software\\Classes\\SystemFileAssociations\\image\\shell\\AWJImage",
      L"Software\\Classes\\SystemFileAssociations\\.jpg\\shell\\AWJImage",
      L"Software\\Classes\\icofile\\shell\\AWJImage",
      L"Software\\Classes\\Directory\\shell\\AWJImage",
  };
  const wchar_t value[] = L"legacy";
  for (const auto path : paths) {
    HKEY raw = nullptr;
    const std::wstring storage{path};
    const auto status = RegCreateKeyExW(HKEY_CURRENT_USER, storage.c_str(), 0, nullptr,
                                        REG_OPTION_NON_VOLATILE, KEY_WRITE,
                                        nullptr, &raw, nullptr);
    if (status != ERROR_SUCCESS) return false;
    const auto set = RegSetValueExW(raw, L"MUIVerb", 0, REG_SZ,
                                    reinterpret_cast<const BYTE*>(value),
                                    sizeof(value));
    RegCloseKey(raw);
    if (set != ERROR_SUCCESS) return false;
  }
  return true;
}

bool write_string(std::wstring_view key, std::wstring_view name,
                  std::wstring_view value) {
  HKEY raw = nullptr;
  const std::wstring path{key};
  const auto created = RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr,
                                       REG_OPTION_NON_VOLATILE, KEY_WRITE,
                                       nullptr, &raw, nullptr);
  if (created != ERROR_SUCCESS) return false;
  const std::wstring value_name{name};
  const std::wstring value_storage{value};
  const auto bytes = static_cast<DWORD>((value_storage.size() + 1) * sizeof(wchar_t));
  const auto status = RegSetValueExW(raw, name.empty() ? nullptr : value_name.c_str(), 0,
                                     REG_SZ,
                                     reinterpret_cast<const BYTE*>(value_storage.c_str()),
                                     bytes);
  RegCloseKey(raw);
  return status == ERROR_SUCCESS;
}

bool write_dword(std::wstring_view key, std::wstring_view name, DWORD value) {
  HKEY raw = nullptr;
  const std::wstring path{key};
  const auto created = RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr,
                                       REG_OPTION_NON_VOLATILE, KEY_WRITE,
                                       nullptr, &raw, nullptr);
  if (created != ERROR_SUCCESS) return false;
  const std::wstring value_name{name};
  const auto status = RegSetValueExW(raw, value_name.c_str(), 0, REG_DWORD,
                                     reinterpret_cast<const BYTE*>(&value), sizeof(value));
  RegCloseKey(raw);
  return status == ERROR_SUCCESS;
}

std::optional<std::wstring> read_string(std::wstring_view key, std::wstring_view name) {
  const std::wstring path{key};
  const std::wstring value_name{name};
  DWORD bytes = 0;
  const auto first = RegGetValueW(HKEY_CURRENT_USER, path.c_str(),
                                  name.empty() ? nullptr : value_name.c_str(),
                                  RRF_RT_REG_SZ, nullptr, nullptr, &bytes);
  if (first != ERROR_SUCCESS) return std::nullopt;
  std::wstring value(bytes / sizeof(wchar_t), L'\0');
  if (RegGetValueW(HKEY_CURRENT_USER, path.c_str(),
                   name.empty() ? nullptr : value_name.c_str(),
                   RRF_RT_REG_SZ, nullptr, value.data(), &bytes) != ERROR_SUCCESS) {
    return std::nullopt;
  }
  while (!value.empty() && value.back() == L'\0') value.pop_back();
  return value;
}

std::wstring quote_arg(std::wstring_view arg) {
  std::wstring out{L"\""};
  std::size_t slashes = 0;
  for (wchar_t ch : arg) {
    if (ch == L'\\') { ++slashes; continue; }
    if (ch == L'\"') {
      out.append(slashes * 2 + 1, L'\\');
      out.push_back(ch);
      slashes = 0;
      continue;
    }
    out.append(slashes, L'\\');
    slashes = 0;
    out.push_back(ch);
  }
  out.append(slashes * 2, L'\\');
  out.push_back(L'\"');
  return out;
}

bool run_command_probe(std::wstring command,
                       const std::vector<std::filesystem::path>& inputs) {
  const auto marker = command.find(L"-i \"%1\" %*");
  if (marker == std::wstring::npos || inputs.empty()) return false;
  std::wstring expanded = command.substr(0, marker);
  expanded += L"-i ";
  expanded += quote_arg(inputs.front().wstring());
  for (std::size_t i = 1; i < inputs.size(); ++i) {
    expanded.push_back(L' ');
    expanded += quote_arg(inputs[i].wstring());
  }
  std::vector<wchar_t> writable(expanded.begin(), expanded.end());
  writable.push_back(L'\0');
  STARTUPINFOW startup{.cb = sizeof(startup)};
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(nullptr, writable.data(), nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
    return false;
  }
  CloseHandle(process.hThread);
  const DWORD wait = WaitForSingleObject(process.hProcess, 30000);
  DWORD exit_code = STILL_ACTIVE;
  if (wait == WAIT_OBJECT_0) GetExitCodeProcess(process.hProcess, &exit_code);
  if (wait != WAIT_OBJECT_0) TerminateProcess(process.hProcess, 0xdead);
  CloseHandle(process.hProcess);
  return wait == WAIT_OBJECT_0 && exit_code == 0;
}

bool write_one_pixel_bmp(const std::filesystem::path& path) {
  constexpr std::array<unsigned char, 58> bmp = {
      0x42, 0x4d, 0x3a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x36, 0x00, 0x00, 0x00, 0x28, 0x00, 0x00, 0x00, 0x01, 0x00,
      0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x18, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x13, 0x0b,
      0x00, 0x00, 0x13, 0x0b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00};
  std::ofstream file(path, std::ios::binary);
  file.write(reinterpret_cast<const char*>(bmp.data()),
             static_cast<std::streamsize>(bmp.size()));
  return file.good();
}

awj::shell_context_menu::MenuParams make_params() {
  awj::shell_context_menu::MenuParams params{};
  for (auto& item : params) {
    item.close_on_finish = true;
    item.allow_wic_fallback = true;
    item.size_limit_index = 0;
  }
  params[0].quality_text = L"75";
  params[0].speed_text = L"6";
  params[0].install_avif_png_command = true;
  params[1].quality_text = L"80";
  params[2].quality_text = L"80";
  params[2].speed_text = L"6";
  params[3].quality_text = L"80";
  return params;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  using namespace awj::shell_context_menu;
  if (argc != 2) return fail("expected AWJ executable path argument");
  const std::filesystem::path awj_exe{argv[1]};
  if (!std::filesystem::is_regular_file(awj_exe)) return fail("AWJ executable is missing");

  const auto params = make_params();
  auto final_cleanup = [] { (void)remove(); };
  if (!seed_legacy_key()) return fail("failed to seed legacy AWJ registry root");

  auto installed = install(awj_exe, params);
  if (!installed) {
    final_cleanup();
    return fail(installed.error());
  }
  for (const auto& legacy : legacy_root_keys()) {
    if (key_exists(legacy)) {
      final_cleanup();
      return fail("legacy AWJ registry root survived migration");
    }
  }

  auto plan = detect_install_plan();
  if (!plan) {
    final_cleanup();
    return fail(plan.error());
  }
  const auto schema = build_registry_schema(awj_exe, params, *plan);
  for (const auto& parent : schema.parent_roots) {
    if (!key_exists(parent)) {
      final_cleanup();
      return fail("expected current parent root is missing");
    }
    const auto pointer = read_string(parent, L"ExtendedSubCommandsKey");
    const auto multi = read_string(parent, L"MultiSelectModel");
    if (!pointer || *pointer != shared_tree_reference || !multi || *multi != L"Player") {
      final_cleanup();
      return fail("parent does not use shared ExtendedSubCommandsKey/Player schema");
    }
  }
  if (key_exists(extension_parent_key(L".jpg")) ||
      key_exists(extension_parent_key(L".png")) ||
      key_exists(extension_parent_key(L".ico")) ||
      key_exists(L"Software\\Classes\\icofile\\shell\\AWJimage.Convert")) {
    final_cleanup();
    return fail("ordinary image/ico duplicate root was installed");
  }
  for (const auto& extension : plan->fallback_extensions) {
    if (!key_exists(extension_parent_key(extension))) {
      final_cleanup();
      return fail("system-required extension fallback root is missing");
    }
  }
  if (!key_exists(directory_parent_key())) {
    final_cleanup();
    return fail("Directory context-menu root is missing");
  }

  auto healthy = warning(awj_exe, params);
  if (!healthy || *healthy) {
    final_cleanup();
    return fail(healthy ? **healthy : healthy.error());
  }

  const auto expect_stale_then_reinstall = [&](std::string_view reason) -> bool {
    auto stale = warning(awj_exe, params);
    if (!stale || !*stale) {
      if (!stale) fail(stale.error());
      else fail(reason);
      return false;
    }
    auto restored = install(awj_exe, params);
    if (!restored) {
      fail(restored.error());
      return false;
    }
    auto restored_warning = warning(awj_exe, params);
    if (!restored_warning || *restored_warning) {
      if (!restored_warning) fail(restored_warning.error());
      else fail("reinstalled shell schema remained stale");
      return false;
    }
    return true;
  };

  if (!write_dword(shared_tree_key(), schema_value_name, schema_version + 1) ||
      !expect_stale_then_reinstall("schema-version drift was not detected")) {
    final_cleanup();
    return 1;
  }
  if (!write_string(image_parent_key(), L"MultiSelectModel", L"Single") ||
      !expect_stale_then_reinstall("MultiSelectModel drift was not detected")) {
    final_cleanup();
    return 1;
  }
  if (!write_string(extension_parent_key(L".jpg"), L"MUIVerb", L"unexpected") ||
      !expect_stale_then_reinstall("unexpected image fallback root was not detected")) {
    final_cleanup();
    return 1;
  }

  auto installed_twice = install(awj_exe, params);
  if (!installed_twice) {
    final_cleanup();
    return fail(installed_twice.error());
  }
  healthy = warning(awj_exe, params);
  if (!healthy || *healthy) {
    final_cleanup();
    return fail(healthy ? **healthy : healthy.error());
  }

  const auto temp = std::filesystem::temp_directory_path() / L"awj-shell-context-menu-registry-test";
  std::error_code ec;
  std::filesystem::remove_all(temp, ec);
  std::filesystem::create_directories(temp, ec);
  if (ec) {
    final_cleanup();
    return fail("failed to create shell test directory");
  }
  std::vector<std::filesystem::path> inputs;
  inputs.reserve(100);
  for (int i = 0; i < 100; ++i) {
    auto path = temp / (L"input-" + std::to_wstring(i) + L".bmp");
    if (!write_one_pixel_bmp(path)) {
      std::filesystem::remove_all(temp, ec);
      final_cleanup();
      return fail("failed to create valid shell test image");
    }
    inputs.push_back(std::move(path));
  }

  const std::vector<std::filesystem::path> one_input{inputs.front()};
  std::wstring png_player_command;
  const auto shared = shared_tree_key();
  for (const auto& spec : command_specs()) {
    const auto command_key = std::format(L"{}\\shell\\{}\\command", shared,
                                         spec.canonical_verb);
    const auto command = read_string(command_key, L"");
    const auto verb_key = std::format(L"{}\\shell\\{}", shared, spec.canonical_verb);
    const auto multi = read_string(verb_key, L"MultiSelectModel");
    if (!command || !multi || *multi != L"Player" ||
        command->find(L"%*") == std::wstring::npos ||
        !run_command_probe(*command, one_input)) {
      std::filesystem::remove_all(temp, ec);
      final_cleanup();
      return fail("registered format command did not safely launch AWJ");
    }
    if (spec.format == L"png" && !spec.append_png_suffix) {
      png_player_command = *command;
    }
  }
  if (png_player_command.empty() || !run_command_probe(png_player_command, inputs)) {
    std::filesystem::remove_all(temp, ec);
    final_cleanup();
    return fail("registered PNG command did not accept 100-item Player arguments");
  }
  std::filesystem::remove_all(temp, ec);

  auto removed = remove();
  if (!removed) return fail(removed.error());
  auto removed_twice = remove();
  if (!removed_twice) return fail(removed_twice.error());
  for (const auto& root : owned_root_keys()) {
    if (key_exists(root)) return fail("AWJ registry residue remained after remove/remove");
  }
  return 0;
}
