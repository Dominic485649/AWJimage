#pragma once

#include "shell_context_menu.hpp"
#include <memory>

namespace awj::shell_context_menu {

class MenuTransaction {
 public:
  explicit MenuTransaction(void* handle) : handle_(handle) {}
  ~MenuTransaction();
  MenuTransaction(const MenuTransaction&) = delete;
  MenuTransaction& operator=(const MenuTransaction&) = delete;
  std::expected<void, std::string> commit();
  void* handle() const noexcept { return handle_; }
 private:
  void* handle_{};
};

std::expected<std::shared_ptr<MenuTransaction>, std::string> prepare_menu_change(
    const std::filesystem::path& exe, const MenuParams& params,
    std::span<const std::wstring> names, bool compatibility, bool remove_menu);
int run_elevation_helper(int argc, wchar_t* argv[]) noexcept;

}  // namespace awj::shell_context_menu
