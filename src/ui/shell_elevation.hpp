#pragma once

#include "shell_context_menu.hpp"
#include <memory>
#include <functional>

namespace awj::shell_context_menu {

class MenuTransaction {
 public:
  struct Impl;
  explicit MenuTransaction(std::unique_ptr<Impl> impl);
  ~MenuTransaction();
  MenuTransaction(const MenuTransaction&) = delete;
  MenuTransaction& operator=(const MenuTransaction&) = delete;
  std::expected<void, std::string> commit();
  std::expected<void, std::string> rollback();
  std::wstring_view id() const noexcept;
  bool machine() const noexcept;
 private:
  std::unique_ptr<Impl> impl_;
};

std::expected<std::shared_ptr<MenuTransaction>, std::string> prepare_menu_change(
    const std::filesystem::path& exe, const MenuParams& params,
    std::span<const std::wstring> names, bool compatibility, bool remove_menu,
    const std::function<void()>& elevation_requested = {});
int run_elevation_helper(int argc, wchar_t* argv[]) noexcept;

}  // namespace awj::shell_context_menu
