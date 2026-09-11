#pragma once

#include <windows.h>
#include <string>
#include <stdexcept>

// A process-local HKCU override: registry unit tests never touch real AWJ menus.
struct IsolatedRegistry {
  HKEY key{};
  std::wstring path = L"Software\\AWJimage.Tests\\" +
      std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
  IsolatedRegistry() {
    DWORD disposition = 0;
    const auto created = RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr,
        0, KEY_ALL_ACCESS, nullptr, &key, &disposition);
    if (created != ERROR_SUCCESS || disposition != REG_CREATED_NEW_KEY) {
      if (key) RegCloseKey(key);
      key = nullptr;
      throw std::runtime_error("cannot create a unique registry sandbox");
    }
    if (RegOverridePredefKey(HKEY_CURRENT_USER, key) != ERROR_SUCCESS) {
      RegCloseKey(key);
      key = nullptr;
      RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str());
      throw std::runtime_error("cannot isolate HKCU");
    }
  }
  ~IsolatedRegistry() {
    RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
    if (key) RegCloseKey(key);
    RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str());
  }
};
