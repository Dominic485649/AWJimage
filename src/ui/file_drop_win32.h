#pragma once

#ifdef _WIN32

#include <windows.h>
#include <oleidl.h>

#include <expected>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace awj::ui_drop {

struct HoverState {
  bool active{};
  bool valid{};
  std::size_t item_count{};
};

struct Callbacks {
  std::function<bool()> can_accept;
  std::function<void(HoverState)> hover_changed;
  std::function<void(std::vector<std::filesystem::path>)> paths_dropped;
};

class Registration {
 public:
  Registration() = default;
  ~Registration();
  Registration(const Registration&) = delete;
  Registration& operator=(const Registration&) = delete;
  Registration(Registration&& other) noexcept;
  Registration& operator=(Registration&& other) noexcept;

  [[nodiscard]] bool active() const noexcept { return registered_; }

 private:
  friend std::expected<Registration, std::string> install(HWND, Callbacks);
  HWND hwnd_{};
  IUnknown* target_{};
  bool ole_initialized_{};
  bool registered_{};
};

// Slint 1.17.1 pins Winit 0.30.13, which registers its default OLE IDropTarget.
// Installation performs a fail-closed ownership transfer: OleInitialize is balanced,
// the expected Winit registration must be revocable, then AWJ registers its own target.
std::expected<Registration, std::string> install(HWND hwnd, Callbacks callbacks);

// Exposed for focused CF_HDROP tests; does no file-system scanning.
std::expected<std::vector<std::filesystem::path>, std::string> extract_hdrop_paths(IDataObject* data_object);
std::expected<std::size_t, std::string> hdrop_item_count(IDataObject* data_object);

}  // namespace awj::ui_drop

#endif
