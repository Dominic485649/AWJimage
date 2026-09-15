#pragma once

#include <expected>
#include <filesystem>
#include <string>

namespace awj::shell_context_menu {

// The sparse package is a per-user registration bridge used by the modern
// Windows Explorer context menu.  The package contains only the manifest and
// assets; code is loaded from the installation directory via
// uap10:AllowExternalContent.
std::filesystem::path sparse_package_path(
    const std::filesystem::path& awj_exe);

std::expected<void, std::string> ensure_sparse_package_registered(
    const std::filesystem::path& awj_exe);
std::expected<void, std::string> remove_sparse_package_registration();

}  // namespace awj::shell_context_menu
