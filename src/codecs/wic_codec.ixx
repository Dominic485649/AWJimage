module;
#include "../compat/stdlib_compat.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <filesystem>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#include <windows.h>
#include <wincodec.h>

export module awj.wic_codec;

import awj.codec;
import awj.core;
import awj.decoder_common;
import awj.encoding_defaults;
import awj.image;
import awj.large_image_plan;

export namespace awj {

namespace wic_detail {

struct ComReleaser {
  template <class T>
  void operator()(T* value) const noexcept {
    if (value != nullptr) {
      value->Release();
    }
  }
};

template <class T>
using ComPtr = std::unique_ptr<T, ComReleaser>;

struct ComApartment {
  ComApartment() noexcept
      : init{CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE)},
        should_uninitialize{SUCCEEDED(init)} {}

  ~ComApartment() {
    if (should_uninitialize) {
      CoUninitialize();
    }
  }

  ComApartment(const ComApartment&) = delete;
  ComApartment& operator=(const ComApartment&) = delete;

  ComApartment(ComApartment&& other) noexcept
      : init{other.init}, should_uninitialize{other.should_uninitialize} {
    other.should_uninitialize = false;
  }

  ComApartment& operator=(ComApartment&& other) noexcept {
    if (this != &other) {
      if (should_uninitialize) {
        CoUninitialize();
      }
      init = other.init;
      should_uninitialize = other.should_uninitialize;
      other.should_uninitialize = false;
    }
    return *this;
  }

  [[nodiscard]] bool usable() const noexcept {
    return SUCCEEDED(init) || init == RPC_E_CHANGED_MODE;
  }

  HRESULT init{S_FALSE};
  bool should_uninitialize{};
};

struct ThreadFactory {
  ComApartment com{};
  ComPtr<IWICImagingFactory> factory{};
  HRESULT status{};
  ThreadFactory() {
    status = com.init;
    if (SUCCEEDED(com.init)) {
      IWICImagingFactory* value = nullptr;
      status = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&value));
      factory.reset(value);
    }
  }
};

struct ProbeFrameResult {
  std::optional<ComApartment> borrowed_com{};
  ComPtr<IWICImagingFactory> owned_factory{};
  IWICImagingFactory* factory{};
  ComPtr<IWICBitmapDecoder> decoder{};
  ComPtr<IWICBitmapFrameDecode> frame{};
};

std::string hresult_message(HRESULT hr) {
  return awj::compat::format("HRESULT 0x{:08X}", static_cast<unsigned int>(hr));
}

awj::compat::expected<void, std::string> check_input_file_size(const fs::path& path) {
  std::error_code ec;
  const auto file_size = fs::file_size(path, ec);
  if (ec) {
    return awj::compat::unexpected{awj::compat::format("读取 WIC 文件大小失败: {}；系统错误：{}",
                                       display_path_for_user(path), ec.message())};
  }
  if (file_size > static_cast<std::uintmax_t>(encoding_defaults::effective_max_input_file_bytes())) {
    return awj::compat::unexpected{awj::compat::format("WIC 文件超过当前输入上限: {}",
                                       display_path_for_user(path))};
  }
  return {};
}

awj::compat::expected<ProbeFrameResult, std::string> open_first_frame(const fs::path& path) {
  if (auto file_size = check_input_file_size(path); !file_size) {
    return awj::compat::unexpected{file_size.error()};
  }

  ProbeFrameResult result{};
  thread_local ThreadFactory cached;
  if (cached.com.init == RPC_E_CHANGED_MODE) {
    // An externally owned STA may end before this thread. Keep its factory
    // scoped to the frame rather than retaining an apartment-bound pointer.
    result.borrowed_com.emplace();
    if (!result.borrowed_com->usable())
      return awj::compat::unexpected{awj::compat::format("WIC COM 初始化失败: {}",
                                         hresult_message(result.borrowed_com->init))};
    IWICImagingFactory* value = nullptr;
    const HRESULT status = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&value));
    result.owned_factory.reset(value);
    if (FAILED(status)) return awj::compat::unexpected{awj::compat::format("WIC 不可用: {}", hresult_message(status))};
    result.factory = result.owned_factory.get();
  } else if (!cached.factory) {
    return awj::compat::unexpected{awj::compat::format("WIC 不可用: {}", hresult_message(cached.status))};
  } else {
    result.factory = cached.factory.get();
  }
  HRESULT hr{};

  IWICBitmapDecoder* raw_decoder = nullptr;
  hr = result.factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                 WICDecodeMetadataCacheOnDemand, &raw_decoder);
  if (FAILED(hr)) {
    return awj::compat::unexpected{awj::compat::format("WIC 打开失败: {}", hresult_message(hr))};
  }
  result.decoder.reset(raw_decoder);

  UINT frame_count = 0;
  hr = result.decoder->GetFrameCount(&frame_count);
  if (FAILED(hr)) {
    return awj::compat::unexpected{awj::compat::format("WIC 读取帧数失败: {}", hresult_message(hr))};
  }
  if (frame_count == 0) {
    return awj::compat::unexpected{"WIC 不包含图像帧。"};
  }
  UINT frame_index = 0;
  static constexpr std::wstring_view ico_extensions[] = {L".ico"};
  if (frame_count > 1 &&
      decoder_common::extension_is_one_of(path, ico_extensions)) {
    std::uint64_t best_area = 0;
    for (UINT i = 0; i < frame_count; ++i) {
      IWICBitmapFrameDecode* candidate = nullptr;
      hr = result.decoder->GetFrame(i, &candidate);
      ComPtr<IWICBitmapFrameDecode> candidate_frame{candidate};
      if (FAILED(hr) || candidate_frame == nullptr) {
        continue;
      }
      UINT width = 0;
      UINT height = 0;
      hr = candidate_frame->GetSize(&width, &height);
      if (FAILED(hr)) {
        continue;
      }
      const auto area = static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
      if (area > best_area) {
        best_area = area;
        frame_index = i;
      }
    }
  }

  IWICBitmapFrameDecode* raw_frame = nullptr;
  hr = result.decoder->GetFrame(frame_index, &raw_frame);
  if (FAILED(hr)) {
    return awj::compat::unexpected{awj::compat::format("WIC 读取首帧失败: {}", hresult_message(hr))};
  }
  result.frame.reset(raw_frame);
  return std::move(result);
}


bool is_rgb_float_format(const WICPixelFormatGUID& format) noexcept {
  return IsEqualGUID(format, GUID_WICPixelFormat48bppRGBHalf) ||
         IsEqualGUID(format, GUID_WICPixelFormat64bppRGBHalf) ||
         IsEqualGUID(format, GUID_WICPixelFormat64bppRGBAHalf) ||
         IsEqualGUID(format, GUID_WICPixelFormat64bppPRGBAHalf) ||
         IsEqualGUID(format, GUID_WICPixelFormat96bppRGBFloat) ||
         IsEqualGUID(format, GUID_WICPixelFormat128bppRGBFloat) ||
         IsEqualGUID(format, GUID_WICPixelFormat128bppRGBAFloat) ||
         IsEqualGUID(format, GUID_WICPixelFormat128bppPRGBAFloat);
}

struct PixelFormatDetails {
  ImageSourceInfo source_info{};
  std::optional<bool> supports_transparency{};
};

PixelFormat pixel_format_from_wic(UINT channel_count,
                                  bool supports_transparency,
                                  WICPixelFormatNumericRepresentation numeric_representation) noexcept {
  if (numeric_representation == WICPixelFormatNumericRepresentationIndexed) {
    return PixelFormat::rgb;
  }
  if (channel_count == 1) {
    return PixelFormat::gray;
  }
  if (channel_count == 3) {
    return PixelFormat::rgb;
  }
  if (channel_count == 4 && supports_transparency) {
    return PixelFormat::rgba;
  }
  return PixelFormat::unknown;
}

int bit_depth_from_wic(UINT bits_per_pixel,
                       UINT channel_count,
                       WICPixelFormatNumericRepresentation numeric_representation) noexcept {
  if (bits_per_pixel == 0 || bits_per_pixel > static_cast<UINT>(std::numeric_limits<int>::max())) {
    return 0;
  }
  if (numeric_representation == WICPixelFormatNumericRepresentationIndexed) {
    return static_cast<int>(bits_per_pixel);
  }
  if (channel_count == 0 || bits_per_pixel % channel_count != 0) {
    return 0;
  }
  const auto depth = bits_per_pixel / channel_count;
  return depth == 0 ? 0 : static_cast<int>(depth);
}

PixelFormatDetails source_info_from_pixel_format(IWICImagingFactory& factory,
                                                 const WICPixelFormatGUID& pixel_format) {
  PixelFormatDetails result{};
  IWICComponentInfo* raw_component_info = nullptr;
  HRESULT hr = factory.CreateComponentInfo(pixel_format, &raw_component_info);
  if (FAILED(hr)) {
    return result;
  }
  ComPtr<IWICComponentInfo> component_info{raw_component_info};

  IWICPixelFormatInfo2* raw_pixel_info = nullptr;
  hr = component_info->QueryInterface(IID_PPV_ARGS(&raw_pixel_info));
  if (FAILED(hr)) {
    return result;
  }
  ComPtr<IWICPixelFormatInfo2> pixel_info{raw_pixel_info};

  BOOL supports_transparency = FALSE;
  hr = pixel_info->SupportsTransparency(&supports_transparency);
  if (SUCCEEDED(hr)) {
    result.supports_transparency = supports_transparency != FALSE;
  }

  UINT bits_per_pixel = 0;
  if (FAILED(pixel_info->GetBitsPerPixel(&bits_per_pixel))) {
    bits_per_pixel = 0;
  }
  UINT channel_count = 0;
  if (FAILED(pixel_info->GetChannelCount(&channel_count))) {
    channel_count = 0;
  }
  WICPixelFormatNumericRepresentation numeric_representation =
      WICPixelFormatNumericRepresentationUnspecified;
  if (FAILED(pixel_info->GetNumericRepresentation(&numeric_representation))) {
    numeric_representation = WICPixelFormatNumericRepresentationUnspecified;
  }
  result.source_info = ImageSourceInfo{
      .pixel_format = pixel_format_from_wic(channel_count,
                                            result.supports_transparency.value_or(false),
                                            numeric_representation),
      .bit_depth = bit_depth_from_wic(bits_per_pixel, channel_count, numeric_representation)};
  return result;
}

awj::compat::expected<std::vector<std::byte>, std::string> copy_icc_profile(
    IWICImagingFactory& factory,
    IWICBitmapFrameDecode& frame) {
  UINT context_count = 0;
  HRESULT hr = frame.GetColorContexts(0, nullptr, &context_count);
  if (FAILED(hr) || context_count == 0) {
    return std::vector<std::byte>{};
  }
  if (context_count >
      static_cast<UINT>(encoding_defaults::wic_max_color_contexts)) {
    return awj::compat::unexpected{"WIC 色彩上下文数量超过 16 个上限。"};
  }

  std::vector<ComPtr<IWICColorContext>> contexts{};
  std::vector<IWICColorContext*> raw_contexts{};
  contexts.reserve(context_count);
  raw_contexts.reserve(context_count);
  for (UINT index = 0; index < context_count; ++index) {
    IWICColorContext* raw_context = nullptr;
    hr = factory.CreateColorContext(&raw_context);
    if (FAILED(hr)) {
      return awj::compat::unexpected{awj::compat::format("WIC 创建色彩上下文失败: {}", hresult_message(hr))};
    }
    contexts.emplace_back(raw_context);
    raw_contexts.push_back(raw_context);
  }

  UINT actual_count = 0;
  hr = frame.GetColorContexts(context_count, raw_contexts.data(), &actual_count);
  if (FAILED(hr)) {
    return std::vector<std::byte>{};
  }
  for (UINT index = 0; index < actual_count && index < raw_contexts.size(); ++index) {
    WICColorContextType type = WICColorContextUninitialized;
    hr = raw_contexts[index]->GetType(&type);
    if (FAILED(hr) || type != WICColorContextProfile) {
      continue;
    }
    UINT profile_size = 0;
    hr = raw_contexts[index]->GetProfileBytes(0, nullptr, &profile_size);
    if (FAILED(hr) || profile_size == 0) {
      continue;
    }
    if (profile_size > encoding_defaults::codec_metadata_max_bytes) {
      return awj::compat::unexpected{"WIC ICC profile 超过 64 MiB 上限。"};
    }
    auto bytes = decoder_common::make_byte_buffer(profile_size, "WIC ICC profile");
    if (!bytes) {
      return awj::compat::unexpected{bytes.error()};
    }
    hr = raw_contexts[index]->GetProfileBytes(profile_size,
                                              reinterpret_cast<BYTE*>(bytes->data()),
                                              &profile_size);
    if (FAILED(hr)) {
      return awj::compat::unexpected{awj::compat::format("WIC 读取 ICC profile 失败: {}", hresult_message(hr))};
    }
    if (profile_size > bytes->size()) {
      return awj::compat::unexpected{"WIC ICC profile 大小在读取期间发生变化。"};
    }
    if (profile_size < bytes->size()) {
      bytes->resize(profile_size);
    }
    return std::move(*bytes);
  }
  return std::vector<std::byte>{};
}

}  // namespace wic_detail

class WicImageDecoder final : public ImageDecoder {
 public:
  [[nodiscard]] std::string_view id() const noexcept override { return "wic"; }

  [[nodiscard]] bool can_decode(const fs::path& path) const override {
    static constexpr std::wstring_view extensions[] = {
        L".bmp", L".dib", L".rle", L".jpg", L".jpeg", L".jpe", L".jfif",
        L".png", L".gif", L".ico", L".tif", L".tiff", L".wdp", L".hdp", L".jxr",
        L".heic", L".heif"};
    return decoder_common::extension_is_one_of(path, extensions);
  }

  awj::compat::expected<ImageDimensions, std::string> probe_dimensions(
      const fs::path& path) const override {
    try {
      auto frame = wic_detail::open_first_frame(path);
      if (!frame) {
        return awj::compat::unexpected{frame.error()};
      }
      UINT width = 0;
      UINT height = 0;
      const HRESULT hr = frame->frame->GetSize(&width, &height);
      if (FAILED(hr)) {
        return awj::compat::unexpected{awj::compat::format("WIC 尺寸无效: {}", wic_detail::hresult_message(hr))};
      }
      return decoder_common::make_image_dimensions_checked(width, height, "WIC");
    } catch (const std::bad_alloc&) {
      return awj::compat::unexpected{"WIC 尺寸探测内存不足。"};
    } catch (const std::length_error&) {
      return awj::compat::unexpected{"WIC 尺寸探测数据超过运行时限制。"};
    } catch (const std::filesystem::filesystem_error&) {
      return awj::compat::unexpected{"WIC 尺寸探测文件系统访问失败。"};
    }
  }

  awj::compat::expected<ImageDecodeResult, std::string> decode(const fs::path& path) const override {
    try {
      auto frame_source = wic_detail::open_first_frame(path);
      if (!frame_source) {
        return awj::compat::unexpected{frame_source.error()};
      }

      UINT width = 0;
      UINT height = 0;
      HRESULT hr = frame_source->frame->GetSize(&width, &height);
      if (FAILED(hr) || width == 0 || height == 0) {
        return awj::compat::unexpected{awj::compat::format("WIC 尺寸无效: {}", wic_detail::hresult_message(hr))};
      }

      WICPixelFormatGUID source_pixel_format{};
      hr = frame_source->frame->GetPixelFormat(&source_pixel_format);
      if (FAILED(hr)) {
        return awj::compat::unexpected{awj::compat::format("WIC 像素格式无效: {}", wic_detail::hresult_message(hr))};
      }

      const auto source_details =
          wic_detail::source_info_from_pixel_format(*frame_source->factory, source_pixel_format);
      auto output_source_info = source_details.source_info;
      // 浮点 RGB 统一保留为现有 FP16 工作像素，不能先经 UNORM 裁切高光和负值。
      const bool source_is_rgb_float = wic_detail::is_rgb_float_format(source_pixel_format);
      if (source_is_rgb_float && !IsEqualGUID(source_pixel_format, GUID_WICPixelFormat64bppRGBAHalf)) {
        UINT context_count = 0;
        hr = frame_source->frame->GetColorContexts(0, nullptr, &context_count);
        if (FAILED(hr) || context_count != 0) {
          return awj::compat::unexpected{"WIC 浮点 RGB 的色彩上下文无法安全解释，不能默认按 scRGB 解码。"};
        }
      }
      const bool wants_high_depth = source_is_rgb_float || output_source_info.bit_depth > 8;
      const auto target_format = source_is_rgb_float ? GUID_WICPixelFormat64bppRGBAHalf
                                 : (wants_high_depth ? GUID_WICPixelFormat64bppRGBA
                                                     : GUID_WICPixelFormat32bppRGBA);
      int output_bit_depth = wants_high_depth ? 16 : 8;

      wic_detail::ComPtr<IWICFormatConverter> converter;
      IWICFormatConverter* raw_converter = nullptr;
      hr = frame_source->factory->CreateFormatConverter(&raw_converter);
      if (FAILED(hr)) {
        return awj::compat::unexpected{awj::compat::format("WIC 创建格式转换器失败: {}", wic_detail::hresult_message(hr))};
      }
      converter.reset(raw_converter);

      hr = converter->Initialize(frame_source->frame.get(), target_format,
                                 WICBitmapDitherTypeNone, nullptr, 0.0,
                                 WICBitmapPaletteTypeCustom);
      if (FAILED(hr) && wants_high_depth && !source_is_rgb_float) {
        output_bit_depth = 8;
        output_source_info.bit_depth = 8;
        output_source_info.has_hdr_metadata = false;
        output_source_info.color_metadata_source = "wic-sdr-fallback";
        hr = converter->Initialize(frame_source->frame.get(), GUID_WICPixelFormat32bppRGBA,
                                   WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeCustom);
      }
      if (FAILED(hr)) {
        return awj::compat::unexpected{awj::compat::format("WIC 转换 RGBA 失败: {}", wic_detail::hresult_message(hr))};
      }

      const auto stride = decoder_common::checked_rgba_stride(
          width, "WIC decoder", output_bit_depth > 8 ? 2 : 1);
      if (!stride) {
        return awj::compat::unexpected{stride.error()};
      }
      const auto byte_count = decoder_common::checked_image_bytes(*stride, height, "WIC decoder");
      if (!byte_count) {
        return awj::compat::unexpected{byte_count.error()};
      }
      if (*stride > std::numeric_limits<UINT>::max() || *byte_count > std::numeric_limits<UINT>::max()) {
        return awj::compat::unexpected{"WIC 输出 buffer 超过 API 限制。"};
      }

      auto rgba = decoder_common::make_byte_buffer(*byte_count, "WIC decoder");
      if (!rgba) {
        return awj::compat::unexpected{rgba.error()};
      }
      hr = converter->CopyPixels(nullptr, static_cast<UINT>(*stride), static_cast<UINT>(rgba->size()),
                                 reinterpret_cast<BYTE*>(rgba->data()));
      if (FAILED(hr)) {
        return awj::compat::unexpected{awj::compat::format("WIC 复制像素失败: {}", wic_detail::hresult_message(hr))};
      }

      const bool retained_scrgb_half = source_is_rgb_float && output_bit_depth == 16;
      if (retained_scrgb_half) {
        output_source_info.pixel_format = PixelFormat::rgba;
        output_source_info.bit_depth = 16;
        output_source_info.color_primaries = 1;
        output_source_info.transfer_characteristics = 8;
        output_source_info.matrix_coefficients = 0;
        output_source_info.color_range = 1;
        output_source_info.has_hdr_metadata = true;
        output_source_info.color_metadata_source = "wic-scrgb-half-linear";
      }

      const auto alpha_mode = source_details.supports_transparency.value_or(true)
                                  ? AlphaMode::straight
                                  : AlphaMode::none;
      auto icc_profile = retained_scrgb_half
                              ? awj::compat::expected<std::vector<std::byte>, std::string>{std::vector<std::byte>{}}
                             : wic_detail::copy_icc_profile(*frame_source->factory,
                                                            *frame_source->frame);
      if (!icc_profile) {
        return awj::compat::unexpected{icc_profile.error()};
      }
      if (!icc_profile->empty()) {
        output_source_info.color_metadata_source = "source-icc";
      }
      auto image = decoder_common::make_rgba_image(width, height, std::move(*rgba), alpha_mode,
                                                   "WIC decoder", output_source_info,
                                                   output_bit_depth,
                                                   retained_scrgb_half
                                                       ? SampleRepresentation::ieee_half_float
                                                       : SampleRepresentation::unorm);
      if (!image) {
        return awj::compat::unexpected{image.error()};
      }
      if (!icc_profile->empty()) {
        image->metadata.push_back(MetadataBlock{.kind = MetadataKind::icc,
                                                .bytes = std::move(*icc_profile)});
      }
      return ImageDecodeResult{.image = std::move(*image), .decoder_id = "wic"};
    } catch (const std::bad_alloc&) {
      return awj::compat::unexpected{"WIC 解码内存不足。"};
    } catch (const std::length_error&) {
      return awj::compat::unexpected{"WIC 解码数据超过运行时限制。"};
    } catch (const std::filesystem::filesystem_error&) {
      return awj::compat::unexpected{"WIC 解码文件系统访问失败。"};
    }
  }
};

}  // namespace awj
