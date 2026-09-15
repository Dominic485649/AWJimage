#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>

#include "shell_extension_contract.hpp"

#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace awj::test {

inline std::optional<std::filesystem::path> modern_configuration_path() {
  PWSTR raw = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData,
                                  KF_FLAG_NO_PACKAGE_REDIRECTION,
                                  nullptr, &raw)) || raw == nullptr) {
    if (raw != nullptr) CoTaskMemFree(raw);
    return std::nullopt;
  }
  std::filesystem::path path{raw};
  CoTaskMemFree(raw);
  path /= L"AWJimage";
  path /= std::wstring{
      awj::shell_extension::contract::modern_configuration_file_name};
  return path;
}

class ModernConfigurationRestore {
 public:
  ModernConfigurationRestore() {
    path_ = modern_configuration_path();
    if (!path_) return;

    std::error_code error;
    existed_ = std::filesystem::is_regular_file(*path_, error) && !error;
    if (!existed_) {
      DeleteFileW(path_->c_str());
      return;
    }

    std::ifstream input(*path_, std::ios::binary);
    if (!input) throw std::runtime_error("could not open modern configuration backup");
    content_ = std::vector<char>{std::istreambuf_iterator<char>{input},
                                 std::istreambuf_iterator<char>{}};
    if (!input.good() && !input.eof()) {
      throw std::runtime_error("could not read modern configuration backup");
    }
    if (!DeleteFileW(path_->c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) {
      throw std::runtime_error("could not isolate modern configuration file");
    }
  }

  ModernConfigurationRestore(const ModernConfigurationRestore&) = delete;
  ModernConfigurationRestore& operator=(const ModernConfigurationRestore&) = delete;

  ~ModernConfigurationRestore() {
    if (!path_) return;
    std::error_code ignored;
    if (!existed_) {
      DeleteFileW(path_->c_str());
      return;
    }

    std::filesystem::create_directories(path_->parent_path(), ignored);
    auto temporary = *path_;
    temporary += L".test-restore.tmp";
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    const DWORD bytes = static_cast<DWORD>(content_->size());
    const BOOL written_ok =
        WriteFile(file, content_->data(), bytes, &written, nullptr);
    if (written_ok && written == bytes) FlushFileBuffers(file);
    CloseHandle(file);
    if (!written_ok || written != bytes ||
        !MoveFileExW(temporary.c_str(), path_->c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
      DeleteFileW(temporary.c_str());
    }
  }

 private:
  std::optional<std::filesystem::path> path_{};
  bool existed_{};
  std::optional<std::vector<char>> content_{};
};

}  // namespace awj::test
