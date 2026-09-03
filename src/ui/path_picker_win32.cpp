#include "path_picker_win32.h"

#ifdef _WIN32

#include <shobjidl.h>
#include <windows.h>

#include <iterator>
#include <memory>

namespace awj::ui_path_picker {
namespace {

class ComApartment {
 public:
  ComApartment()
      : result_(CoInitializeEx(nullptr,
                               COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)) {}

  ~ComApartment() {
    if (SUCCEEDED(result_)) {
      CoUninitialize();
    }
  }

  [[nodiscard]] bool usable() const noexcept {
    return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE;
  }

 private:
  HRESULT result_{};
};

template <class Interface>
struct ComReleaseDeleter {
  void operator()(Interface* value) const noexcept {
    if (value != nullptr) {
      value->Release();
    }
  }
};

struct CoTaskMemDeleter {
  void operator()(wchar_t* value) const noexcept {
    if (value != nullptr) {
      CoTaskMemFree(value);
    }
  }
};

enum class PathPickerMode { image_file, folder };

std::optional<std::filesystem::path> choose_path(PathPickerMode mode) {
  ComApartment apartment;
  if (!apartment.usable()) {
    return std::nullopt;
  }

  IFileDialog* raw_dialog = nullptr;
  HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&raw_dialog));
  std::unique_ptr<IFileDialog, ComReleaseDeleter<IFileDialog>> dialog{raw_dialog};
  if (FAILED(hr) || dialog == nullptr) {
    return std::nullopt;
  }

  DWORD options{};
  hr = dialog->GetOptions(&options);
  if (FAILED(hr)) {
    return std::nullopt;
  }
  options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
  options |= mode == PathPickerMode::folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST;
  hr = dialog->SetOptions(options);
  if (FAILED(hr)) {
    return std::nullopt;
  }

  if (mode == PathPickerMode::image_file) {
    const COMDLG_FILTERSPEC filters[] = {
        {L"图片文件",
         L"*.jpg;*.jpeg;*.jpe;*.jfif;*.png;*.webp;*.bmp;*.dib;*.rle;*.tif;*."
         L"tiff;*.gif;*.ico;*.jxl;*.avif;*.awsraw;*.dng;*.cr2;*.cr3;*.nef;*.arw;*."
         L"rw2;*.orf;*.raf;*.pef;*.srw;*.x3f;*.3fr;*.erf;*.kdc;*.mrw;*.raw;*."
         L"heic;*.heif;*.jxr;*.wdp;*.hdp"},
        {L"所有文件", L"*.*"}};
    hr = dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
    if (FAILED(hr)) {
      return std::nullopt;
    }
  }

  hr = dialog->Show(nullptr);
  if (FAILED(hr)) {
    return std::nullopt;
  }

  IShellItem* raw_item = nullptr;
  hr = dialog->GetResult(&raw_item);
  std::unique_ptr<IShellItem, ComReleaseDeleter<IShellItem>> item{raw_item};
  if (FAILED(hr) || item == nullptr) {
    return std::nullopt;
  }

  PWSTR raw_path = nullptr;
  hr = item->GetDisplayName(SIGDN_FILESYSPATH, &raw_path);
  std::unique_ptr<wchar_t, CoTaskMemDeleter> path{raw_path};
  if (FAILED(hr) || path == nullptr) {
    return std::nullopt;
  }

  return std::filesystem::path{path.get()};
}

}  // namespace

std::optional<std::filesystem::path> choose_path(bool pick_folder) {
  return choose_path(pick_folder ? PathPickerMode::folder : PathPickerMode::image_file);
}

}  // namespace awj::ui_path_picker

#endif
