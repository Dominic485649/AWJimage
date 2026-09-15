#include <windows.h>
#include <shlobj.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <expected>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

import awj.preset;

namespace fs = std::filesystem;

void check(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}

template <class T>
T require(std::expected<T, std::string> value) {
  if (!value) throw std::runtime_error(value.error());
  return std::move(*value);
}

fs::path executable_directory() {
  std::wstring buffer(MAX_PATH, L'\0');
  while (true) {
    const DWORD written = GetModuleFileNameW(nullptr, buffer.data(),
                                              static_cast<DWORD>(buffer.size()));
    check(written != 0, "failed to locate fallback test executable");
    if (written + 1 < buffer.size()) {
      buffer.resize(written);
      return fs::path{buffer}.parent_path();
    }
    buffer.resize(buffer.size() * 2, L'\0');
    check(buffer.size() <= 1024u * 1024u, "fallback test executable path is too long");
  }
}

fs::path local_preset_directory() {
  std::wstring environment(256, L'\0');
  DWORD written = GetEnvironmentVariableW(
      L"AWJIMAGE_TEST_LOCAL_APPDATA", environment.data(),
      static_cast<DWORD>(environment.size()));
  if (written >= environment.size()) {
    environment.resize(static_cast<std::size_t>(written) + 1, L'\0');
    written = GetEnvironmentVariableW(
        L"AWJIMAGE_TEST_LOCAL_APPDATA", environment.data(),
        static_cast<DWORD>(environment.size()));
  }
  if (written != 0) {
    environment.resize(written);
    return fs::path{environment} / L"AWJimage" / L"preset";
  }
  PWSTR raw = nullptr;
  const HRESULT result = SHGetKnownFolderPath(
      FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &raw);
  check(SUCCEEDED(result) && raw != nullptr, "failed to locate LocalAppData");
  fs::path root{raw};
  CoTaskMemFree(raw);
  return root / L"AWJimage" / L"preset";
}

int run_icacls(const fs::path& path, std::wstring_view arguments) {
  const auto command = L"icacls.exe \"" + path.wstring() + L"\" " +
                       std::wstring{arguments};
  return _wsystem(command.c_str());
}

struct AclRestore {
  fs::path path;
  bool active{};
  ~AclRestore() {
    if (active) (void)run_icacls(path, L"/reset /T /C");
  }
};

int main() try {
  const auto adjacent = executable_directory() / L"preset";
  const auto local = local_preset_directory();
  check(!fs::exists(adjacent), "fallback test directory was not isolated");
  const bool local_preexisting = fs::exists(local);
  if (local_preexisting) {
    check(fs::is_directory(local), "existing LocalAppData preset path is not a directory");
  }

  fs::create_directories(adjacent);
  const auto suffix = std::to_wstring(GetCurrentProcessId());
  const auto legacy = adjacent / (L"fallback-legacy-" + suffix + L".jsonc");
  {
    std::ofstream output{legacy, std::ios::binary | std::ios::trunc};
    check(static_cast<bool>(output), "failed to create legacy preset");
    output << R"({"schema":1,"name":"fallback legacy","description":"","formats":{}})";
  }

  AclRestore acl{adjacent, false};
  // Keep inherited read/list access, but deny the directory operations used
  // by the write probe and atomic preset replacement.
  check(run_icacls(adjacent, L"/deny *S-1-1-0:(WD,AD,WA,WEA,DC)") == 0,
        "failed to make adjacent preset directory read-only");
  acl.active = true;

  const auto selected = require(awj::user_preset_directory());
  check(selected == local, "protected adjacent preset directory was selected for writes");
  const auto migrated = local / legacy.filename();
  check(fs::exists(migrated), "legacy preset was not migrated to LocalAppData");
  const auto loaded = require(awj::load_user_preset_file(migrated));
  check(loaded.name == "fallback legacy", "migrated preset contents changed");

  // Restore the ACL before deleting the adjacent test directory.  Only remove
  // the unique file we copied when LocalAppData was already in use by a user.
  acl.active = false;
  check(run_icacls(adjacent, L"/reset /T /C") == 0,
        "failed to restore adjacent preset ACL");
  std::error_code ec;
  fs::remove_all(adjacent, ec);
  check(!ec, "failed to clean adjacent fallback test directory");
  if (local_preexisting) {
    fs::remove(migrated, ec);
    check(!ec, "failed to clean migrated LocalAppData preset");
  } else {
    fs::remove_all(local, ec);
    check(!ec, "failed to clean isolated LocalAppData preset directory");
  }
  std::puts("Protected adjacent preset fallback and migration passed.");
  return 0;
} catch (const std::exception& error) {
  std::fprintf(stderr, "%s\n", error.what());
  return 1;
}
