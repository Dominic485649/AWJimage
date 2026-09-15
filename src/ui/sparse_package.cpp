#include "sparse_package.hpp"

#include "shell_extension_contract.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Management.Deployment.h>
#include <winrt/Windows.Storage.h>
#include <winrt/base.h>

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <future>
#include <format>
#include <string_view>
#include <type_traits>

namespace awj::shell_context_menu {
namespace {

#ifndef AWJ_SPARSE_PACKAGE_MAJOR
#define AWJ_SPARSE_PACKAGE_MAJOR 0
#define AWJ_SPARSE_PACKAGE_MINOR 0
#define AWJ_SPARSE_PACKAGE_BUILD 0
#define AWJ_SPARSE_PACKAGE_REVISION 0
#endif

constexpr std::wstring_view kPackageName = L"AWJimage.ContextMenu";

std::string hresult_error_text(std::string_view operation,
                               const winrt::hresult_error& error) {
  return std::format("{}失败：HRESULT 0x{:08X}，{}。", operation,
                     static_cast<unsigned>(error.code().value),
                     winrt::to_string(error.message()));
}

std::string deployment_error_text(
    std::string_view operation,
    const winrt::Windows::Management::Deployment::DeploymentResult& result) {
  const auto code = result.ExtendedErrorCode();
  const auto message = winrt::to_string(result.ErrorText());
  if (message.empty()) {
    return std::format("{}失败：HRESULT 0x{:08X}。", operation,
                       static_cast<unsigned>(code.value));
  }
  return std::format("{}失败：HRESULT 0x{:08X}，{}。", operation,
                     static_cast<unsigned>(code.value), message);
}

std::wstring normalized_path(const std::filesystem::path& path) {
  std::error_code error;
  auto result = std::filesystem::weakly_canonical(path, error);
  if (error) result = std::filesystem::absolute(path, error);
  if (error) result = path;
  result = result.lexically_normal();
  auto text = result.wstring();
  std::ranges::transform(text, text.begin(), [](wchar_t character) {
    return character == L'/' ? L'\\' : static_cast<wchar_t>(std::towlower(character));
  });
  while (text.size() > 3 && !text.empty() && text.back() == L'\\') text.pop_back();
  return text;
}

bool external_location_matches(
    const winrt::Windows::ApplicationModel::Package& package,
    const std::filesystem::path& expected) {
  try {
    const auto location = package.UserExternalLocation();
    if (!location) return false;
    return normalized_path(location.Path().c_str()) == normalized_path(expected);
  } catch (...) {
    // Older package registrations may not expose the UserExternalLocation
    // property. Treat them as stale and re-register them below.
    return false;
  }
}

bool package_version_matches(
    const winrt::Windows::ApplicationModel::Package& package) {
  try {
    const auto version = package.Id().Version();
    return version.Major == AWJ_SPARSE_PACKAGE_MAJOR &&
           version.Minor == AWJ_SPARSE_PACKAGE_MINOR &&
           version.Build == AWJ_SPARSE_PACKAGE_BUILD &&
           version.Revision == AWJ_SPARSE_PACKAGE_REVISION;
  } catch (...) {
    return false;
  }
}

template <class Action>
auto run_on_mta(Action&& action) {
  using Result = std::invoke_result_t<Action>;
  return std::async(std::launch::async, [fn = std::forward<Action>(action)]() mutable
      -> Result {
    bool initialized = false;
    try {
      winrt::init_apartment(winrt::apartment_type::multi_threaded);
      initialized = true;
      auto result = fn();
      if (initialized) winrt::uninit_apartment();
      return result;
    } catch (...) {
      if (initialized) winrt::uninit_apartment();
      throw;
    }
  }).get();
}

std::expected<void, std::string> remove_named_packages(
    winrt::Windows::Management::Deployment::PackageManager& manager) {
  using namespace winrt::Windows::Management::Deployment;
  try {
    for (const auto& package : manager.FindPackagesForUser(L"")) {
      if (package.Id().Name() != kPackageName) continue;
      const auto operation = manager.RemovePackageAsync(
          package.Id().FullName(), RemovalOptions::None);
      const auto result = operation.get();
      if (!SUCCEEDED(result.ExtendedErrorCode())) {
        return std::unexpected{deployment_error_text("移除 AWJimage Explorer 包", result)};
      }
    }
  } catch (const winrt::hresult_error& error) {
    return std::unexpected{hresult_error_text("查找或移除 AWJimage Explorer 包", error)};
  } catch (const std::exception& error) {
    return std::unexpected{std::format("查找或移除 AWJimage Explorer 包失败：{}。",
                                       error.what())};
  }
  return {};
}

}  // namespace

std::filesystem::path sparse_package_path(
    const std::filesystem::path& awj_exe) {
  return awj_exe.parent_path() / L"AWJ.ContextMenu.msix";
}

std::expected<void, std::string> ensure_sparse_package_registered(
    const std::filesystem::path& awj_exe) {
  const auto package_path = sparse_package_path(awj_exe);
  std::error_code error;
  if (!std::filesystem::is_regular_file(package_path, error) || error) {
    // Development builds or older portable archives may not carry the optional
    // sparse package. Their classic HKCU menu remains fully functional.
    return {};
  }
  const auto external_directory = awj_exe.parent_path();

  try {
    return run_on_mta([package_path, external_directory]()
        -> std::expected<void, std::string> {
      namespace foundation = winrt::Windows::Foundation;
      namespace deployment = winrt::Windows::Management::Deployment;
      try {
        deployment::PackageManager manager;
        bool current = false;
        for (const auto& package : manager.FindPackagesForUser(L"")) {
          if (package.Id().Name() != kPackageName) continue;
          if (external_location_matches(package, external_directory) &&
              package_version_matches(package)) {
            current = true;
            break;
          }
        }
        if (current) return {};

        if (auto removed = remove_named_packages(manager); !removed) {
          return removed;
        }

        // Windows.Foundation.Uri accepts an absolute Windows path for local
        // package deployment, matching the API usage in TortoiseGit's sparse
        // package registration implementation.
        const foundation::Uri package_uri(package_path.wstring());
        const foundation::Uri external_uri(external_directory.wstring());
        deployment::AddPackageOptions options;
        options.ExternalLocationUri(external_uri);
        const auto result = manager.AddPackageByUriAsync(package_uri, options).get();
        if (!SUCCEEDED(result.ExtendedErrorCode())) {
          return std::unexpected{deployment_error_text("注册 AWJimage Explorer 包", result)};
        }
      } catch (const winrt::hresult_error& caught) {
        return std::unexpected{hresult_error_text("注册 AWJimage Explorer 包", caught)};
      } catch (const std::exception& caught) {
        return std::unexpected{std::format("注册 AWJimage Explorer 包失败：{}。",
                                           caught.what())};
      }
      return {};
    });
  } catch (const winrt::hresult_error& error_value) {
    return std::unexpected{hresult_error_text("初始化 Explorer 包注册线程", error_value)};
  } catch (const std::exception& error_value) {
    return std::unexpected{std::format("初始化 Explorer 包注册线程失败：{}。",
                                       error_value.what())};
  }
}

std::expected<void, std::string> remove_sparse_package_registration() {
  try {
    return run_on_mta([]() -> std::expected<void, std::string> {
      namespace deployment = winrt::Windows::Management::Deployment;
      try {
        deployment::PackageManager manager;
        return remove_named_packages(manager);
      } catch (const winrt::hresult_error& caught) {
        return std::unexpected{hresult_error_text("移除 AWJimage Explorer 包", caught)};
      } catch (const std::exception& caught) {
        return std::unexpected{std::format("移除 AWJimage Explorer 包失败：{}。",
                                           caught.what())};
      }
    });
  } catch (const winrt::hresult_error& error_value) {
    return std::unexpected{hresult_error_text("初始化 Explorer 包注销线程", error_value)};
  } catch (const std::exception& error_value) {
    return std::unexpected{std::format("初始化 Explorer 包注销线程失败：{}。",
                                       error_value.what())};
  }
}

}  // namespace awj::shell_context_menu
