#ifdef _WIN32

#include "file_drop_win32.h"

#include <ole2.h>
#include <shellapi.h>

#include <atomic>
#include <cstdint>
#include <format>
#include <new>
#include <utility>

namespace awj::ui_drop {
namespace {

std::string hresult_text(std::string_view action, HRESULT hr) {
  return std::format("{}失败，HRESULT=0x{:08X}", action,
                     static_cast<std::uint32_t>(hr));
}

FORMATETC hdrop_format() noexcept {
  return FORMATETC{.cfFormat = CF_HDROP,
                   .ptd = nullptr,
                   .dwAspect = DVASPECT_CONTENT,
                   .lindex = -1,
                   .tymed = TYMED_HGLOBAL};
}

struct MediumGuard {
  STGMEDIUM medium{};
  bool owns{};
  ~MediumGuard() {
    if (owns) ReleaseStgMedium(&medium);
  }
};

std::expected<HDROP, std::string> get_hdrop(IDataObject* data_object,
                                             MediumGuard& guard) {
  if (data_object == nullptr) return std::unexpected{"拖放数据对象为空。"};
  auto format = hdrop_format();
  const HRESULT hr = data_object->GetData(&format, &guard.medium);
  if (FAILED(hr)) return std::unexpected{hresult_text("读取 CF_HDROP", hr)};
  guard.owns = true;
  if (guard.medium.tymed != TYMED_HGLOBAL || guard.medium.hGlobal == nullptr) {
    return std::unexpected{"CF_HDROP 未返回 TYMED_HGLOBAL。"};
  }
  return reinterpret_cast<HDROP>(guard.medium.hGlobal);
}

class DropTarget final : public IDropTarget {
 public:
  DropTarget(HWND hwnd, Callbacks callbacks)
      : hwnd_(hwnd), callbacks_(std::move(callbacks)) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
    if (object == nullptr) return E_POINTER;
    *object = nullptr;
    if (riid == IID_IUnknown || riid == IID_IDropTarget) {
      *object = static_cast<IDropTarget*>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  ULONG STDMETHODCALLTYPE AddRef() override {
    return ref_count_.fetch_add(1, std::memory_order_relaxed) + 1;
  }

  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG remaining = ref_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (remaining == 0) delete this;
    return remaining;
  }

  HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data_object, DWORD,
                                      POINTL position, DWORD* effect) override {
    item_count_ = 0;
    if (auto value = hdrop_item_count(data_object)) item_count_ = *value;
    const DWORD allowed_effects = effect != nullptr ? *effect : DROPEFFECT_NONE;
    const auto feedback = feedback_at(position, allowed_effects, effect != nullptr);
    valid_ = feedback.hover.valid;
    if (effect != nullptr) *effect = feedback.effect;
    notify(feedback.hover);
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL position, DWORD* effect) override {
    const DWORD allowed_effects = effect != nullptr ? *effect : DROPEFFECT_NONE;
    const auto feedback = feedback_at(position, allowed_effects, effect != nullptr);
    valid_ = feedback.hover.valid;
    if (effect != nullptr) *effect = feedback.effect;
    notify(feedback.hover);
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE DragLeave() override {
    clear_hover();
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE Drop(IDataObject* data_object, DWORD, POINTL position,
                                 DWORD* effect) override {
    const DWORD allowed_effects = effect != nullptr ? *effect : DROPEFFECT_NONE;
    const auto feedback = feedback_at(position, allowed_effects, effect != nullptr);
    const bool acceptable = feedback.hover.valid &&
                            static_cast<bool>(callbacks_.paths_dropped);
    const DWORD result = dispatch_copy_drop(allowed_effects, acceptable, [&] {
      auto paths = extract_hdrop_paths(data_object);
      if (!paths || paths->empty()) return false;
      return callbacks_.paths_dropped(std::move(*paths), feedback.hover.target);
    });
    if (effect != nullptr) *effect = result;
    clear_hover();
    return S_OK;
  }

 private:
  DragFeedback feedback_at(POINTL position, DWORD allowed, bool has_effect) noexcept {
    int target = 0;
    try {
      POINT client{position.x, position.y};
      if (has_effect && can_accept_now() && ScreenToClient(hwnd_, &client) &&
          callbacks_.target_at) {
        target = callbacks_.target_at(client, item_count_);
      }
    } catch (...) {
      target = 0;
    }
    auto feedback = copy_drag_feedback(allowed, target != 0, item_count_);
    feedback.hover.target = feedback.hover.valid ? target : 0;
    return feedback;
  }

  bool can_accept_now() noexcept {
    try {
      return !callbacks_.can_accept || callbacks_.can_accept();
    } catch (...) {
      return false;
    }
  }

  void notify(HoverState state) noexcept {
    try {
      if (callbacks_.hover_changed) callbacks_.hover_changed(state);
    } catch (...) {
      // COM callbacks must not unwind into OLE.
    }
  }

  void clear_hover() noexcept {
    valid_ = false;
    item_count_ = 0;
    notify({});
  }

  std::atomic<ULONG> ref_count_{1};
  HWND hwnd_{};
  Callbacks callbacks_;
  std::size_t item_count_{};
  bool valid_{};
};

}  // namespace

InstallRetryAction install_retry_action(bool hwnd_ready,
                                        std::size_t attempts_so_far,
                                        std::size_t max_attempts) noexcept {
  if (hwnd_ready) return InstallRetryAction::install;
  if (max_attempts == 0 || attempts_so_far + 1 >= max_attempts) {
    return InstallRetryAction::exhausted;
  }
  return InstallRetryAction::retry;
}

DWORD select_copy_effect(DWORD allowed_effects, bool acceptable) noexcept {
  return acceptable && (allowed_effects & DROPEFFECT_COPY) != 0 ? DROPEFFECT_COPY
                                                                : DROPEFFECT_NONE;
}

DragFeedback copy_drag_feedback(DWORD allowed_effects, bool can_accept,
                                std::size_t item_count) noexcept {
  const DWORD effect =
      select_copy_effect(allowed_effects, item_count > 0 && can_accept);
  return DragFeedback{
      .effect = effect,
      .hover = HoverState{.active = true,
                          .valid = effect == DROPEFFECT_COPY,
                          .item_count = item_count}};
}

std::expected<std::size_t, std::string> hdrop_item_count(IDataObject* data_object) {
  MediumGuard guard;
  auto hdrop = get_hdrop(data_object, guard);
  if (!hdrop) return std::unexpected{hdrop.error()};
  return static_cast<std::size_t>(DragQueryFileW(*hdrop, 0xFFFFFFFFu, nullptr, 0));
}

std::expected<std::vector<std::filesystem::path>, std::string> extract_hdrop_paths(
    IDataObject* data_object) {
  MediumGuard guard;
  auto hdrop = get_hdrop(data_object, guard);
  if (!hdrop) return std::unexpected{hdrop.error()};

  try {
    const UINT count = DragQueryFileW(*hdrop, 0xFFFFFFFFu, nullptr, 0);
    std::vector<std::filesystem::path> paths;
    paths.reserve(count);
    for (UINT index = 0; index < count; ++index) {
      const UINT length = DragQueryFileW(*hdrop, index, nullptr, 0);
      if (length == 0) continue;
      std::wstring buffer(static_cast<std::size_t>(length) + 1, L'\0');
      const UINT written = DragQueryFileW(*hdrop, index, buffer.data(), length + 1);
      if (written != length) {
        return std::unexpected{"读取 CF_HDROP 文件路径失败。"};
      }
      buffer.resize(length);
      paths.emplace_back(std::move(buffer));
    }
    return paths;
  } catch (const std::bad_alloc&) {
    return std::unexpected{"读取拖放路径时内存不足。"};
  }
}

Registration::~Registration() {
  if (registered_ && hwnd_ != nullptr) {
    RevokeDragDrop(hwnd_);
  }
  if (target_ != nullptr) {
    target_->Release();
  }
  if (ole_initialized_) {
    OleUninitialize();
  }
}

Registration::Registration(Registration&& other) noexcept
    : hwnd_(std::exchange(other.hwnd_, nullptr)),
      target_(std::exchange(other.target_, nullptr)),
      ole_initialized_(std::exchange(other.ole_initialized_, false)),
      registered_(std::exchange(other.registered_, false)) {}

Registration& Registration::operator=(Registration&& other) noexcept {
  if (this == &other) return *this;
  this->~Registration();
  new (this) Registration(std::move(other));
  return *this;
}

std::expected<Registration, std::string> install(HWND hwnd, Callbacks callbacks) {
  if (hwnd == nullptr || !IsWindow(hwnd)) {
    return std::unexpected{"无法为无效 HWND 注册 Windows 原生拖放。"};
  }

  Registration registration;
  registration.hwnd_ = hwnd;
  const HRESULT ole_hr = OleInitialize(nullptr);
  if (FAILED(ole_hr)) {
    return std::unexpected{hresult_text("OleInitialize", ole_hr)};
  }
  registration.ole_initialized_ = true;

  // Slint 1.17.1 pins Winit 0.30.13. That exact backend defaults native drag/drop on,
  // owns the first RegisterDragDrop call, and later revokes it during WM_DESTROY.
  // Do not register over an unknown target: ownership transfer proceeds only if the
  // expected Winit registration can be revoked successfully.
  const HRESULT revoke_hr = RevokeDragDrop(hwnd);
  if (revoke_hr != S_OK) {
    return std::unexpected{hresult_text("撤销 Winit 默认拖放目标", revoke_hr)};
  }

  auto* target = new (std::nothrow) DropTarget(hwnd, std::move(callbacks));
  if (target == nullptr) return std::unexpected{"创建 Windows IDropTarget 时内存不足。"};
  registration.target_ = target;

  const HRESULT register_hr = RegisterDragDrop(hwnd, target);
  if (register_hr != S_OK) {
    return std::unexpected{hresult_text("注册 AWJ Windows 拖放目标", register_hr)};
  }
  registration.registered_ = true;
  return registration;
}

}  // namespace awj::ui_drop

#endif
