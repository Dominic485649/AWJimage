module;
#include "../compat/stdlib_compat.h"
#include "avif_worker_failure.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <numeric>
#include <new>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <avif/avif.h>
#include "avif_primary_item_adapter.h"

module awj.avif_aom_codec;

import awj.avif_registry;
import awj.animation;
import awj.codec;
import awj.config;
import awj.core;
import awj.decoder_common;
import awj.encoding_defaults;
import awj.gain_map;
import awj.hdr_tonemap;
import awj.image;
import awj.pixel_conversion;
import awj.large_image_plan;
import awj.resource_planner;

namespace awj {

namespace avif_aom_detail {

using Clock = std::chrono::steady_clock;

double elapsed_seconds(Clock::time_point started) {
  return std::chrono::duration<double>(Clock::now() - started).count();
}

struct AvifImageDeleter {
  void operator()(avifImage* value) const noexcept {
    if (value != nullptr) {
      avifImageDestroy(value);
    }
  }
};

struct AvifEncoderDeleter {
  void operator()(avifEncoder* value) const noexcept {
    if (value != nullptr) {
      avifEncoderDestroy(value);
    }
  }
};

struct AvifDecoderDeleter {
  void operator()(avifDecoder* value) const noexcept {
    if (value != nullptr) {
      avifDecoderDestroy(value);
    }
  }
};

struct AvifRwDataDeleter {
  void operator()(avifRWData* value) const noexcept {
    if (value != nullptr) {
      avifRWDataFree(value);
      delete value;
    }
  }
};

using AvifImage = std::unique_ptr<avifImage, AvifImageDeleter>;
using AvifEncoder = std::unique_ptr<avifEncoder, AvifEncoderDeleter>;
using AvifDecoder = std::unique_ptr<avifDecoder, AvifDecoderDeleter>;
using AvifRwData = std::unique_ptr<avifRWData, AvifRwDataDeleter>;

awj::compat::expected<void, std::string> stop_if_requested(awj::compat::stop_token stop_token) {
  if (stop_token.stop_requested()) {
    return awj::compat::unexpected{"任务已取消。"};
  }
  return {};
}

awj::compat::expected<AvifRwData, std::string> make_avif_rw_data() {
  try {
    auto data = std::make_unique<avifRWData>();
    data->data = nullptr;
    data->size = 0;
    return AvifRwData{data.release()};
  } catch (const std::bad_alloc&) {
    return awj::compat::unexpected{"AVIF 输出缓冲区内存不足。"};
  }
}

struct AvifFileIO {
  avifIO io{};
  std::ifstream input;
  std::vector<std::uint8_t> buffer;
};

avifResult avif_file_io_read(avifIO* io,
                             uint32_t read_flags,
                             uint64_t offset,
                             size_t size,
                             avifROData* out) {
  if (io == nullptr || out == nullptr || io->data == nullptr) {
    return AVIF_RESULT_INVALID_ARGUMENT;
  }
  if (read_flags != 0) {
    return AVIF_RESULT_IO_ERROR;
  }
  auto& file_io = *static_cast<AvifFileIO*>(io->data);
  if (offset > file_io.io.sizeHint) {
    return AVIF_RESULT_IO_ERROR;
  }
  const auto available = file_io.io.sizeHint - offset;
  const auto bytes_to_read = static_cast<std::size_t>(std::min<std::uint64_t>(available, size));
  if (bytes_to_read == 0) {
    out->data = nullptr;
    out->size = 0;
    return AVIF_RESULT_OK;
  }
  if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max()) ||
      static_cast<std::uint64_t>(bytes_to_read) >
          static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max())) {
    return AVIF_RESULT_IO_ERROR;
  }
  try {
    file_io.buffer.resize(bytes_to_read);
  } catch (const std::bad_alloc&) {
    return AVIF_RESULT_OUT_OF_MEMORY;
  } catch (const std::length_error&) {
    return AVIF_RESULT_IO_ERROR;
  }
  file_io.input.clear();
  file_io.input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
  if (!file_io.input) {
    return AVIF_RESULT_IO_ERROR;
  }
  file_io.input.read(reinterpret_cast<char*>(file_io.buffer.data()),
                     static_cast<std::streamsize>(file_io.buffer.size()));
  if (file_io.input.gcount() != static_cast<std::streamsize>(bytes_to_read)) {
    return AVIF_RESULT_IO_ERROR;
  }
  out->data = file_io.buffer.data();
  out->size = file_io.buffer.size();
  return AVIF_RESULT_OK;
}

awj::compat::expected<std::unique_ptr<AvifFileIO>, std::string> make_avif_file_io(const fs::path& path) {
  auto file_io = std::make_unique<AvifFileIO>();
  file_io->input.open(path, std::ios::binary);
  if (!file_io->input) {
    return awj::compat::unexpected{awj::compat::format("无法读取 AVIF 文件: {}", display_path_for_user(path))};
  }
  file_io->input.seekg(0, std::ios::end);
  if (!file_io->input) {
    return awj::compat::unexpected{awj::compat::format("读取 AVIF 文件大小失败: {}", display_path_for_user(path))};
  }
  const auto size = file_io->input.tellg();
  if (size < 0) {
    return awj::compat::unexpected{awj::compat::format("读取 AVIF 文件大小失败: {}", display_path_for_user(path))};
  }
  if (size == 0) {
    return awj::compat::unexpected{awj::compat::format("AVIF 文件为空: {}", display_path_for_user(path))};
  }
  const auto file_size = static_cast<std::uint64_t>(size);
  if (file_size > encoding_defaults::effective_max_input_file_bytes()) {
    return awj::compat::unexpected{awj::compat::format(
        "AVIF 文件超过当前输入上限: {}", display_path_for_user(path))};
  }
  file_io->input.seekg(0, std::ios::beg);
  if (!file_io->input) {
    return awj::compat::unexpected{awj::compat::format("读取 AVIF 文件失败: {}", display_path_for_user(path))};
  }
  file_io->io.read = avif_file_io_read;
  file_io->io.sizeHint = file_size;
  file_io->io.persistent = AVIF_FALSE;
  file_io->io.data = file_io.get();
  return file_io;
}

awj::compat::expected<std::vector<std::byte>, std::string> read_file_bytes(
    const fs::path& path) {
  return decoder_common::read_file_bytes(path, "AVIF");
}

awj::compat::expected<std::size_t, std::string> checked_interleaved_stride(std::size_t width,
                                                                 std::size_t channels,
                                                                 std::string_view context,
                                                                 std::size_t bytes_per_sample = 1) {
  if (channels == 0 || bytes_per_sample == 0 || width == 0) {
    return awj::compat::unexpected{awj::compat::format("{} 输入宽度无效。", context)};
  }
  if (width > std::numeric_limits<std::size_t>::max() / channels / bytes_per_sample) {
    return awj::compat::unexpected{awj::compat::format("{} 输入宽度过大。", context)};
  }
  const auto stride = width * channels * bytes_per_sample;
  if (stride > std::numeric_limits<std::uint32_t>::max()) {
    return awj::compat::unexpected{awj::compat::format("{} 输入 stride 超出 libavif 限制。", context)};
  }
  return stride;
}

awj::compat::expected<std::size_t, std::string> checked_rgba_stride(std::size_t width,
                                                            std::string_view context,
                                                            std::size_t bytes_per_sample = 1) {
  return checked_interleaved_stride(width, 4, context, bytes_per_sample);
}

awj::compat::expected<std::size_t, std::string> checked_image_bytes(std::size_t stride,
                                                           std::size_t height,
                                                           std::string_view context) {
  if (stride == 0 || height == 0) {
    return awj::compat::unexpected{awj::compat::format("{} 输入尺寸无效。", context)};
  }
  if (height > std::numeric_limits<std::size_t>::max() / stride) {
    return awj::compat::unexpected{awj::compat::format("{} 输入尺寸过大。", context)};
  }
  const auto byte_count = stride * height;
  if (static_cast<std::uint64_t>(byte_count) > encoding_defaults::effective_max_input_file_bytes()) {
    return awj::compat::unexpected{awj::compat::format("{} 图像 buffer 超过当前运行时上限。", context)};
  }
  return byte_count;
}

awj::compat::expected<std::size_t, std::string> checked_strided_rgba_bytes(
    std::size_t width,
    std::size_t height,
    std::size_t stride,
    std::string_view context,
    std::size_t bytes_per_sample = 1) {
  const auto row_bytes = checked_rgba_stride(width, context, bytes_per_sample);
  if (!row_bytes) {
    return awj::compat::unexpected{row_bytes.error()};
  }
  if (height == 0 || stride < *row_bytes) {
    return awj::compat::unexpected{awj::compat::format("{} 输入 RGBA buffer 尺寸无效。", context)};
  }
  if ((height - 1) > (std::numeric_limits<std::size_t>::max() - *row_bytes) / stride) {
    return awj::compat::unexpected{awj::compat::format("{} 输入尺寸过大。", context)};
  }
  const auto byte_count = (height - 1) * stride + *row_bytes;
  if (static_cast<std::uint64_t>(byte_count) > encoding_defaults::effective_max_input_file_bytes()) {
    return awj::compat::unexpected{awj::compat::format("{} RGBA buffer 超过当前运行时上限。", context)};
  }
  return byte_count;
}

awj::compat::expected<const ImagePlane*, std::string> rgba_plane(const ImageBuffer& image,
                                                         std::string_view encoder_id) {
  if (image.pixel_format != PixelFormat::rgba ||
      (image.bit_depth != 8 && image.bit_depth != 16) ||
      image.planes.empty()) {
    return awj::compat::unexpected{awj::compat::format("{} encoder 当前需要 RGBA ImageBuffer。", encoder_id)};
  }
  const auto& plane = image.planes.front();
  const auto bytes_per_sample = image.bit_depth > 8 ? std::size_t{2} : std::size_t{1};
  const auto expected_stride = checked_rgba_stride(image.width, encoder_id, bytes_per_sample);
  if (!expected_stride) {
    return awj::compat::unexpected{expected_stride.error()};
  }
  const auto expected_bytes = checked_image_bytes(plane.stride, image.height, encoder_id);
  if (!expected_bytes) {
    return awj::compat::unexpected{expected_bytes.error()};
  }
  if (!plane.contains_rows(image.height, *expected_stride)) {
    return awj::compat::unexpected{awj::compat::format("{} encoder 输入 RGBA buffer 尺寸无效。", encoder_id)};
  }
  return &plane;
}

awj::compat::expected<avifPixelFormat, std::string> avif_pixel_format_from_chroma(
    ChromaMode chroma) {
  switch (chroma) {
    case ChromaMode::yuv420:
      return AVIF_PIXEL_FORMAT_YUV420;
    case ChromaMode::yuv422:
      return AVIF_PIXEL_FORMAT_YUV422;
    case ChromaMode::yuv444:
    case ChromaMode::auto_keep:
      return AVIF_PIXEL_FORMAT_YUV444;
    case ChromaMode::yuv400:
      return AVIF_PIXEL_FORMAT_YUV400;
    default:
      return awj::compat::unexpected{"AVIF encoder 色度采样参数无效。"};
  }
}

ChromaMode applied_chroma_from_settings(const ImageBuffer& image,
                                        ChromaMode chroma) noexcept {
  return chroma != ChromaMode::auto_keep
             ? chroma
             : avif_auto_chroma(image.source_info ? image.source_info->pixel_format
                                                  : image.pixel_format);
}

const MetadataBlock* first_metadata(const ImageBuffer& image, MetadataKind kind) noexcept {
  for (const auto& block : image.metadata) {
    if (block.kind == kind && !block.bytes.empty()) {
      return &block;
    }
  }
  return nullptr;
}

const MetadataBlock* first_icc_metadata(const ImageBuffer& image) noexcept {
  return first_metadata(image, MetadataKind::icc);
}

awj::compat::expected<void, std::string> set_avif_metadata(
    avifResult result,
    std::string_view kind) {
  if (result != AVIF_RESULT_OK) {
    return awj::compat::unexpected{awj::compat::format("AVIF 设置 {} 元数据失败: {}",
                                       kind, avifResultToString(result))};
  }
  return {};
}

awj::compat::expected<void, std::string> ensure_metadata_size(std::size_t size,
                                                      std::string_view context) {
  if (size > encoding_defaults::codec_metadata_max_bytes) {
    return awj::compat::unexpected{awj::compat::format("AVIF {} 元数据超过 64 MiB 上限。", context)};
  }
  return {};
}

awj::compat::expected<void, std::string> apply_icc_profile(avifImage& avif_image,
                                                   const ImageBuffer& image,
                                                   const NativeEncodeSettings& settings) {
  if (settings.applied_icc != "kept") {
    return {};
  }
  const auto* icc = first_icc_metadata(image);
  if (icc == nullptr) {
    return {};
  }
  if (auto checked = ensure_metadata_size(icc->bytes.size(), "ICC profile"); !checked) {
    return awj::compat::unexpected{checked.error()};
  }
  const auto result = avifImageSetProfileICC(
      &avif_image, reinterpret_cast<const std::uint8_t*>(icc->bytes.data()), icc->bytes.size());
  if (result != AVIF_RESULT_OK) {
    return awj::compat::unexpected{awj::compat::format("AVIF 设置 ICC profile 失败: {}", avifResultToString(result))};
  }
  return {};
}

awj::compat::expected<void, std::string> apply_content_light_metadata(avifImage& avif_image,
                                                              const NativeEncodeSettings& settings) {
  const bool reconstructed = settings.applied_hdr_metadata == "gain-map-reconstructed";
  if ((!reconstructed && settings.strip_metadata) || (!reconstructed && settings.applied_hdr_metadata != "kept") ||
      !settings.source_content_light) {
    return {};
  }
  avif_image.clli.maxCLL = settings.source_content_light->max_cll;
  avif_image.clli.maxPALL = settings.source_content_light->max_pall;
  return {};
}

awj::compat::expected<void, std::string> apply_icc_and_content_light_metadata(
    avifImage& avif_image,
    const ImageBuffer& image,
    const NativeEncodeSettings& settings) {
  if (auto icc = apply_icc_profile(avif_image, image, settings); !icc) {
    return awj::compat::unexpected{icc.error()};
  }
  if (auto content_light = apply_content_light_metadata(avif_image, settings); !content_light) {
    return awj::compat::unexpected{content_light.error()};
  }
  return {};
}

awj::compat::expected<void, std::string> apply_avif_metadata(avifImage& avif_image,
                                                       const ImageBuffer& image,
                                                       const NativeEncodeSettings& settings) {
  if (auto base_metadata = apply_icc_and_content_light_metadata(avif_image, image, settings); !base_metadata) {
    return awj::compat::unexpected{base_metadata.error()};
  }
  if (settings.strip_metadata) {
    return {};
  }
  if (const auto* exif = first_metadata(image, MetadataKind::exif)) {
    if (auto checked = ensure_metadata_size(exif->bytes.size(), "Exif"); !checked) {
      return awj::compat::unexpected{checked.error()};
    }
    if (auto set = set_avif_metadata(
            avifImageSetMetadataExif(&avif_image,
                                      reinterpret_cast<const std::uint8_t*>(exif->bytes.data()),
                                      exif->bytes.size()),
            "Exif"); !set) {
      return awj::compat::unexpected{set.error()};
    }
  }
  if (const auto* xmp = first_metadata(image, MetadataKind::xmp)) {
    if (auto checked = ensure_metadata_size(xmp->bytes.size(), "XMP"); !checked) {
      return awj::compat::unexpected{checked.error()};
    }
    if (auto set = set_avif_metadata(
            avifImageSetMetadataXMP(&avif_image,
                                    reinterpret_cast<const std::uint8_t*>(xmp->bytes.data()),
                                    xmp->bytes.size()),
            "XMP"); !set) {
      return awj::compat::unexpected{set.error()};
    }
  }
  return {};
}

std::optional<int> avif_bit_depth_from_source(const ImageBuffer& image) noexcept {
  if (!image.source_info || image.source_info->bit_depth <= 0) {
    return {};
  }
  const int depth = image.source_info->bit_depth;
  if (depth == 8 || depth == 10 || depth == 12) {
    return depth;
  }
  return {};
}

bool preserve_alpha_for_encode(const NativeEncodeSettings& settings) noexcept {
  return settings.source_has_alpha_channel && settings.encoder_supports_alpha &&
         settings.applied_alpha == "kept";
}

std::optional<int> color_value_for_encode(std::optional<int> value,
                                          int unspecified,
                                          bool preserve_unspecified = false) noexcept {
  if (!value || (!preserve_unspecified && *value == unspecified)) {
    return {};
  }
  return value;
}

int codec_thread_count(int requested_threads) noexcept {
  return std::clamp(requested_threads, 1,
                    encoding_defaults::max_automatic_thread_budget);
}

awj::compat::expected<void, std::string> validate_optional_int_range(std::optional<int> value,
                                                             int min_value,
                                                             int max_value,
                                                             std::string_view name) {
  if (!value) {
    return {};
  }
  if (*value < min_value || *value > max_value) {
    return awj::compat::unexpected{
        awj::compat::format("{} 范围必须在 {} 到 {} 之间。", name, min_value, max_value)};
  }
  return {};
}

awj::compat::expected<void, std::string> validate_avif_color_settings(
    const NativeEncodeSettings& settings) {
  if (auto valid = validate_optional_int_range(
          settings.applied_color_primaries, 0, 255, "color-primaries"); !valid) {
    return awj::compat::unexpected{valid.error()};
  }
  if (auto valid = validate_optional_int_range(
          settings.applied_transfer_characteristics, 0, 255, "transfer-characteristics"); !valid) {
    return awj::compat::unexpected{valid.error()};
  }
  if (auto valid = validate_optional_int_range(
          settings.applied_matrix_coefficients, 0, 255, "matrix-coefficients"); !valid) {
    return awj::compat::unexpected{valid.error()};
  }
  if (auto valid = validate_optional_int_range(settings.applied_color_range, 0, 1, "color-range");
      !valid) {
    return awj::compat::unexpected{valid.error()};
  }
  return {};
}

avifColorPrimaries color_primaries_for_encode(const NativeEncodeSettings& settings,
                                               bool lossless) noexcept {
  if (const auto value = color_value_for_encode(settings.applied_color_primaries,
                                                AVIF_COLOR_PRIMARIES_UNSPECIFIED,
                                                settings.color_metadata_source == "user-cicp-settings")) {
    return static_cast<avifColorPrimaries>(*value);
  }
  if (lossless || settings.applied_icc == "kept") {
    return AVIF_COLOR_PRIMARIES_UNSPECIFIED;
  }
  return AVIF_COLOR_PRIMARIES_BT709;
}

avifTransferCharacteristics transfer_characteristics_for_encode(
    const NativeEncodeSettings& settings,
    bool lossless) noexcept {
  if (const auto value = color_value_for_encode(settings.applied_transfer_characteristics,
                                                AVIF_TRANSFER_CHARACTERISTICS_UNSPECIFIED,
                                                settings.color_metadata_source == "user-cicp-settings")) {
    return static_cast<avifTransferCharacteristics>(*value);
  }
  if (lossless || settings.applied_icc == "kept") {
    return AVIF_TRANSFER_CHARACTERISTICS_UNSPECIFIED;
  }
  return AVIF_TRANSFER_CHARACTERISTICS_SRGB;
}

avifRange range_for_encode(std::optional<int> range) noexcept {
  if (!range) {
    return AVIF_RANGE_FULL;
  }
  return *range == 0 ? AVIF_RANGE_LIMITED : AVIF_RANGE_FULL;
}

avifMatrixCoefficients matrix_coefficients_for_encode(const NativeEncodeSettings& settings,
                                                      ChromaMode chroma,
                                                      bool lossless) noexcept;

void apply_color_settings(avifImage& avif_image,
                          const NativeEncodeSettings& settings,
                          ChromaMode chroma,
                          bool lossless) noexcept {
  avif_image.colorPrimaries = color_primaries_for_encode(settings, lossless);
  avif_image.transferCharacteristics = transfer_characteristics_for_encode(settings, lossless);
  avif_image.matrixCoefficients = matrix_coefficients_for_encode(settings, chroma, lossless);
  avif_image.yuvRange = range_for_encode(settings.applied_color_range);
}

std::optional<int> int_from_avif_color(avifColorPrimaries value) noexcept {
  return value == AVIF_COLOR_PRIMARIES_UNSPECIFIED
             ? std::optional<int>{}
             : std::optional<int>{static_cast<int>(value)};
}

std::optional<int> int_from_avif_transfer(avifTransferCharacteristics value) noexcept {
  return value == AVIF_TRANSFER_CHARACTERISTICS_UNSPECIFIED
             ? std::optional<int>{}
             : std::optional<int>{static_cast<int>(value)};
}

std::optional<int> int_from_avif_matrix(avifMatrixCoefficients value) noexcept {
  return value == AVIF_MATRIX_COEFFICIENTS_UNSPECIFIED
             ? std::optional<int>{}
             : std::optional<int>{static_cast<int>(value)};
}

std::optional<int> int_from_avif_range(avifRange value) noexcept {
  switch (value) {
    case AVIF_RANGE_LIMITED:
      return 0;
    case AVIF_RANGE_FULL:
      return 1;
    default:
      return {};
  }
}

bool has_avif_icc(const avifImage& image) noexcept {
  return image.icc.size > 0 && image.icc.data != nullptr;
}

awj::compat::expected<void, std::string> copy_avif_metadata(ImageBuffer& out,
                                                    MetadataKind kind,
                                                    const avifRWData& metadata) {
  if (metadata.size == 0 || metadata.data == nullptr) {
    return {};
  }
  MetadataBlock block{.kind = kind};
  if (auto checked = ensure_metadata_size(metadata.size, "metadata"); !checked) {
    return awj::compat::unexpected{checked.error()};
  }
  auto bytes = decoder_common::make_byte_buffer(metadata.size, "AVIF metadata");
  if (!bytes) {
    return awj::compat::unexpected{bytes.error()};
  }
  block.bytes = std::move(*bytes);
  awj::compat::ranges::copy_n(reinterpret_cast<const std::byte*>(metadata.data), metadata.size,
                      block.bytes.begin());
  try {
    out.metadata.push_back(std::move(block));
  } catch (const std::bad_alloc&) {
    return awj::compat::unexpected{"AVIF metadata list 内存不足。"};
  } catch (const std::length_error&) {
    return awj::compat::unexpected{"AVIF metadata list 尺寸超过运行时限制。"};
  }
  return {};
}

awj::compat::expected<void, std::string> copy_avif_metadata(ImageBuffer& out,
                                                   const avifImage& image,
                                                   bool copy_payloads = true) {
  if (!copy_payloads) {
    return {};
  }
  if (has_avif_icc(image)) {
    out.source_info->color_metadata_source = "source-icc";
    if (auto copied = copy_avif_metadata(out, MetadataKind::icc, image.icc); !copied) {
      return awj::compat::unexpected{copied.error()};
    }
  }
  if (auto copied = copy_avif_metadata(out, MetadataKind::exif, image.exif); !copied) {
    return awj::compat::unexpected{copied.error()};
  }
  if (auto copied = copy_avif_metadata(out, MetadataKind::xmp, image.xmp); !copied) {
    return awj::compat::unexpected{copied.error()};
  }
  return {};
}

bool has_hdr_cicp(const avifImage& image) noexcept {
  return static_cast<int>(image.colorPrimaries) == 9 ||
         static_cast<int>(image.transferCharacteristics) == 16 ||
         static_cast<int>(image.transferCharacteristics) == 18;
}

std::optional<HdrContentLightMetadata> content_light_from_avif(const avifImage& image) noexcept {
  if (image.clli.maxCLL == 0 && image.clli.maxPALL == 0) {
    return {};
  }
  return HdrContentLightMetadata{.max_cll = image.clli.maxCLL,
                                 .max_pall = image.clli.maxPALL};
}

bool has_hdr_metadata(const avifImage& image) noexcept {
  return has_hdr_cicp(image) || content_light_from_avif(image).has_value();
}

std::string color_metadata_source_from_avif(const avifImage& image) {
  if (has_avif_icc(image)) {
    return "source-icc";
  }
  if (image.colorPrimaries != AVIF_COLOR_PRIMARIES_UNSPECIFIED ||
      image.transferCharacteristics != AVIF_TRANSFER_CHARACTERISTICS_UNSPECIFIED ||
      image.matrixCoefficients != AVIF_MATRIX_COEFFICIENTS_UNSPECIFIED) {
    return "source-cicp";
  }
  return "unknown";
}

awj::compat::expected<void, std::string> apply_display_geometry(ImageBuffer& output, const avifImage& source) {
  const auto flags = source.transformFlags & (AVIF_TRANSFORM_CLAP | AVIF_TRANSFORM_IROT | AVIF_TRANSFORM_IMIR);
  if (!flags) return {};
  ImageGeometry geometry{.stored_width = source.width, .stored_height = source.height,
      .crop_width = source.width, .crop_height = source.height};
  if (flags & AVIF_TRANSFORM_CLAP) {
    avifCropRect crop{}; avifDiagnostics diagnostics{};
    if (!avifCropRectConvertCleanApertureBox(&crop, &source.clap, source.width, source.height, source.yuvFormat, &diagnostics))
      return awj::compat::unexpected{"AVIF clean aperture 无效。"};
    geometry.crop_x = crop.x; geometry.crop_y = crop.y;
    geometry.crop_width = crop.width; geometry.crop_height = crop.height;
  }
  geometry.rotation = flags & AVIF_TRANSFORM_IROT ? source.irot.angle : 0;
  geometry.mirror_axis = flags & AVIF_TRANSFORM_IMIR ? source.imir.axis : -1;
  output.width = geometry.rotation % 2 ? geometry.crop_height : geometry.crop_width;
  output.height = geometry.rotation % 2 ? geometry.crop_width : geometry.crop_height;
  output.geometry = geometry;
  if (!valid_image_geometry(output)) return awj::compat::unexpected{"AVIF 几何属性无效。"};
  return {};
}

int applied_bit_depth_from_settings(const ImageBuffer& image,
                                    const NativeEncodeSettings& settings,
                                    bool lossless) {
  if (settings.bit_depth) {
    return *settings.bit_depth;
  }
  const auto source_depth = avif_bit_depth_from_source(image);
  if (lossless) {
    if (source_depth) {
      return std::max(10, *source_depth);
    }
    return std::max(10, image.bit_depth);
  }
  if (source_depth && *source_depth >= 10) {
    return std::min(12, *source_depth);
  }
  return 10;
}

std::string default_bit_depth_reason(const ImageBuffer& image,
                                     const NativeEncodeSettings& settings,
                                     bool lossless) {
  if (settings.bit_depth_explicit) {
    return "用户明确请求 bit-depth";
  }
  const auto source_depth = avif_bit_depth_from_source(image);
  if (lossless && source_depth && *source_depth < 10) {
    return "无损 auto 将低于 10-bit 的源图提升为 10-bit 输出";
  }
  if (lossless && source_depth) {
    return "无损模式继承源图 bit-depth";
  }
  if (lossless) {
    return image.bit_depth < 10
               ? "无损 auto 将低于 10-bit 的解码图提升为 10-bit 输出"
               : "无损模式继承解码后 bit-depth";
  }
  if (source_depth && *source_depth >= 10 && *source_depth <= 12) {
    return awj::compat::format("有损 auto 保留源图 {}-bit 输出", *source_depth);
  }
  if (source_depth && *source_depth > 12) {
    return awj::compat::format("有损 auto 将源图 {}-bit 限制为 AOM 支持的 12-bit 输出",
                       *source_depth);
  }
  return "auto 选择首选 10-bit 输出";
}

avifMatrixCoefficients matrix_coefficients_for_encode(const NativeEncodeSettings& settings,
                                                      ChromaMode /*chroma*/,
                                                      bool /*lossless*/) noexcept {
  if (settings.avif_color_representation ==
      AvifColorRepresentation::rgb_identity) {
    return AVIF_MATRIX_COEFFICIENTS_IDENTITY;
  }
  if (settings.avif_color_representation == AvifColorRepresentation::yuv) {
    if (settings.applied_matrix_coefficients &&
        *settings.applied_matrix_coefficients != 0 &&
        *settings.applied_matrix_coefficients !=
            AVIF_MATRIX_COEFFICIENTS_UNSPECIFIED) {
      return static_cast<avifMatrixCoefficients>(
          *settings.applied_matrix_coefficients);
    }
    return static_cast<avifMatrixCoefficients>(
        settings.applied_color_primaries == 9
            ? AVIF_MATRIX_COEFFICIENTS_BT2020_NCL
            : AVIF_MATRIX_COEFFICIENTS_BT709);
  }
  if (const auto value = color_value_for_encode(settings.applied_matrix_coefficients,
                                                AVIF_MATRIX_COEFFICIENTS_UNSPECIFIED,
                                                settings.color_metadata_source == "user-cicp-settings")) {
    return static_cast<avifMatrixCoefficients>(*value);
  }
  return static_cast<avifMatrixCoefficients>(
      settings.applied_color_primaries == 9
          ? AVIF_MATRIX_COEFFICIENTS_BT2020_NCL
          : AVIF_MATRIX_COEFFICIENTS_BT709);
}



struct RgbSource {
  avifRGBImage rgb{};
};

awj::compat::expected<RgbSource, std::string> rgb_source_for_encode(
    std::size_t width,
    std::size_t height,
    std::span<const std::byte> pixels,
    std::size_t stride,
    avifImage* avif_image,
    const NativeEncodeSettings& settings,
    int bit_depth);

struct GridTileContext {
  const ImageBuffer* image{};
  const ImagePlane* plane{};
  const NativeEncodeSettings* settings{};
  const GridPlan* plan{};
  avifPixelFormat pixel_format{};
  ChromaMode applied_chroma{};
  bool lossless{};
  int bit_depth{};
  const avifGainMap* gain_map{};
};

awj::compat::expected<void, std::string> attach_grid_gain_map(
    avifImage& tile, const avifGainMap* global, const avifCropRect& crop) {
  if (!global) return {};
  if (!global->image || global->altICC.data || global->altICC.size)
    return awj::compat::unexpected{"Gain Map: ISO 21496-1: Grid 全局增益图状态无效。"};
  IsoGainMapPtr map{avifGainMapCreate(), &avifGainMapDestroy};
  if (!map) return awj::compat::unexpected{"Gain Map: ISO 21496-1: 无法创建 Grid cell metadata。"};
  // 固定版本中仅 image/altICC 拥有动态存储，后者已明确排除。所有 cell 共享同一套数值元数据。
  *map = *global;
  map->image = avifImageCreateEmpty();
  if (!map->image) return awj::compat::unexpected{"Gain Map: ISO 21496-1: 无法创建 Grid gain view。"};
  const auto result = avifImageSetViewRect(map->image, global->image, &crop);
  if (result != AVIF_RESULT_OK)
    return awj::compat::unexpected{"Gain Map: ISO 21496-1: Grid 增益图裁切失败: " + std::string{avifResultToString(result)}};
  map->altPlaneCount = 3;
  tile.gainMap = map.release();
  return {};
}

awj::compat::expected<AvifImage, std::string> prepare_grid_tile(
    const GridTileContext& context,
    std::size_t tile_index,
    awj::compat::stop_token stop_token) {
  if (context.image == nullptr || context.plane == nullptr ||
      context.settings == nullptr || context.plan == nullptr) {
    return awj::compat::unexpected{"AVIF grid tile 上下文无效。"};
  }
  if (auto stopped = stop_if_requested(stop_token); !stopped) {
    return awj::compat::unexpected{stopped.error()};
  }
  const auto& image = *context.image;
  const auto& plane = *context.plane;
  const auto& settings = *context.settings;
  const auto& plan = *context.plan;
  const auto cols = static_cast<std::size_t>(plan.cols);
  if (cols == 0) {
    return awj::compat::unexpected{"AVIF grid 规划无效。"};
  }
  const auto row = tile_index / cols;
  const auto col = tile_index % cols;
  if (row > std::numeric_limits<std::uint32_t>::max() ||
      col > std::numeric_limits<std::uint32_t>::max()) {
    return awj::compat::unexpected{"AVIF grid tile 索引超过运行时限制。"};
  }
  const std::size_t src_x = col * plan.tile_width;
  const std::size_t src_y = row * plan.tile_height;
  if (src_x >= image.width || src_y >= image.height) {
    return awj::compat::unexpected{"AVIF grid tile 范围超出输入图片。"};
  }
  const auto tile_width = std::min<std::size_t>(plan.tile_width, image.width - src_x);
  const auto tile_height = std::min<std::size_t>(plan.tile_height, image.height - src_y);
  if (is_yuv_format(image.pixel_format)) {
    auto view = yuv_plane_view(image);
    if (!view) return awj::compat::unexpected{view.error()};
    auto tile = AvifImage{avifImageCreateEmpty()};
    if (!tile) return awj::compat::unexpected{"无法创建 Grid YUV view。"};
    const avifCropRect crop{static_cast<std::uint32_t>(src_x), static_cast<std::uint32_t>(src_y),
        static_cast<std::uint32_t>(tile_width), static_cast<std::uint32_t>(tile_height)};
    const auto result = avifImageSetViewRect(tile.get(), &*view, &crop);
    if (result != AVIF_RESULT_OK) return awj::compat::unexpected{std::string{"Grid YUV view 失败: "} + avifResultToString(result)};
    apply_color_settings(*tile, settings, context.applied_chroma, context.lossless);
    auto metadata = tile_index == 0 ? apply_avif_metadata(*tile, image, settings)
        : apply_icc_and_content_light_metadata(*tile, image, settings);
    if (!metadata) return awj::compat::unexpected{metadata.error()};
    if (auto gain = attach_grid_gain_map(*tile, context.gain_map, crop); !gain) return awj::compat::unexpected{gain.error()};
    return tile;
  }
  if (src_x > std::numeric_limits<std::size_t>::max() / 4 ||
      src_y > std::numeric_limits<std::size_t>::max() / plane.stride) {
    return awj::compat::unexpected{"AVIF grid tile 偏移过大。"};
  }
  const auto row_offset = src_y * plane.stride;
  const auto bytes_per_sample = image.bit_depth > 8 ? std::size_t{2} : std::size_t{1};
  const auto col_offset = src_x * 4 * bytes_per_sample;
  if (col_offset > std::numeric_limits<std::size_t>::max() - row_offset) {
    return awj::compat::unexpected{"AVIF grid tile 偏移过大。"};
  }
  const auto tile_offset = row_offset + col_offset;
  if (tile_offset > plane.read_bytes().size()) {
    return awj::compat::unexpected{"AVIF grid tile 输入范围无效。"};
  }
  const auto tile_pixels = std::span<const std::byte>{
      plane.read_bytes().data() + tile_offset, plane.read_bytes().size() - tile_offset};
  auto tile = AvifImage{avifImageCreate(
      static_cast<std::uint32_t>(tile_width), static_cast<std::uint32_t>(tile_height),
      context.bit_depth, context.pixel_format)};
  if (!tile) {
    return awj::compat::unexpected{"无法创建 AVIF grid tile。"};
  }
  apply_color_settings(*tile, settings, context.applied_chroma, context.lossless);
  if (tile_index == 0) {
    if (auto metadata = apply_avif_metadata(*tile, image, settings); !metadata) {
      return awj::compat::unexpected{metadata.error()};
    }
  } else {
    if (auto metadata = apply_icc_and_content_light_metadata(*tile, image, settings);
        !metadata) {
      return awj::compat::unexpected{metadata.error()};
    }
  }
  auto rgb = rgb_source_for_encode(
      tile_width, tile_height, tile_pixels,
      plane.stride, tile.get(), settings,
      image.bit_depth);
  if (!rgb) {
    return awj::compat::unexpected{rgb.error()};
  }
  if (auto stopped = stop_if_requested(stop_token); !stopped) {
    return awj::compat::unexpected{stopped.error()};
  }
  const auto result = avifImageRGBToYUV(tile.get(), &rgb->rgb);
  if (auto stopped = stop_if_requested(stop_token); !stopped) {
    return awj::compat::unexpected{stopped.error()};
  }
  if (result != AVIF_RESULT_OK) {
    return awj::compat::unexpected{awj::compat::format("AVIF grid tile RGB 转 YUV 失败: {}",
                                      avifResultToString(result))};
  }
  const avifCropRect crop{static_cast<std::uint32_t>(src_x), static_cast<std::uint32_t>(src_y),
      static_cast<std::uint32_t>(tile_width), static_cast<std::uint32_t>(tile_height)};
  if (auto gain = attach_grid_gain_map(*tile, context.gain_map, crop); !gain) return awj::compat::unexpected{gain.error()};
  return tile;
}

int grid_tile_prepare_thread_count(std::size_t tile_count,
                                   const ResourcePlan& resources) noexcept {
  if (tile_count <= 1) {
    return 1;
  }
  const int hardware = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
  const int budget = std::max(1, resources.global_thread_budget);
  const int file_parallelism = std::max(1, resources.file_parallelism);
  const int limit = std::min({budget, file_parallelism, hardware,
                              static_cast<int>(std::min<std::size_t>(
                                  tile_count, static_cast<std::size_t>(std::numeric_limits<int>::max())))});
  return std::max(1, limit);
}

awj::compat::expected<void, std::string> prepare_grid_tiles_serial(
    const GridTileContext& context,
    std::vector<AvifImage>& tile_storage,
    std::vector<const avifImage*>& tile_views,
    awj::compat::stop_token stop_token) {
  for (std::size_t index = 0; index < tile_storage.size(); ++index) {
    auto tile = prepare_grid_tile(context, index, stop_token);
    if (!tile) {
      return awj::compat::unexpected{tile.error()};
    }
    tile_storage[index] = std::move(*tile);
    tile_views[index] = tile_storage[index].get();
  }
  return {};
}

awj::compat::expected<void, std::string> prepare_grid_tiles_parallel(
    const GridTileContext& context,
    std::vector<AvifImage>& tile_storage,
    std::vector<const avifImage*>& tile_views,
    int thread_count,
    awj::compat::stop_token stop_token) {
  if (thread_count <= 1 || tile_storage.size() <= 1) {
    return prepare_grid_tiles_serial(context, tile_storage, tile_views, stop_token);
  }

  using TileFailure = avif_worker::GridTileFailure;
  std::atomic<std::size_t> next_tile{0};
  avif_worker::GridTileFailureState failure;
  std::vector<awj::compat::scoped_worker> workers;
  try {
    workers.reserve(static_cast<std::size_t>(thread_count));
    for (int worker = 0; worker < thread_count; ++worker) {
      workers.emplace_back([&]() noexcept {
        try {
          while (failure.reason() == TileFailure::none &&
                 !stop_token.stop_requested()) {
            const auto index = next_tile.fetch_add(1, std::memory_order_relaxed);
            if (index >= tile_storage.size()) return;
            auto tile = prepare_grid_tile(context, index, stop_token);
            if (!tile) {
              // 仅首个失败者写错误文本；swap 不分配，caller 在 join 后才读取。
              failure.capture(tile.error());
              return;
            }
            tile_storage[index] = std::move(*tile);
            tile_views[index] = tile_storage[index].get();
          }
        } catch (const std::bad_alloc&) {
          failure.record(TileFailure::memory);
        } catch (const std::length_error&) {
          failure.record(TileFailure::length);
        } catch (const std::exception&) {
          failure.record(TileFailure::worker);
        } catch (...) {
          failure.record(TileFailure::unknown);
        }
      });
    }
  } catch (const std::bad_alloc&) {
    workers.clear();
    return prepare_grid_tiles_serial(context, tile_storage, tile_views, stop_token);
  } catch (const std::system_error&) {
    workers.clear();
    return prepare_grid_tiles_serial(context, tile_storage, tile_views, stop_token);
  } catch (const std::length_error&) {
    workers.clear();
    return prepare_grid_tiles_serial(context, tile_storage, tile_views, stop_token);
  }

  workers.clear();
  if (stop_token.stop_requested()) return awj::compat::unexpected{"任务已取消。"};
  const char* message = "AVIF grid tile 准备失败。";
  switch (failure.reason()) {
    case TileFailure::none: return {};
    case TileFailure::tile: {
      auto error_message = failure.take_error();
      if (!error_message.empty()) return awj::compat::unexpected{std::move(error_message)};
      break;
    }
    case TileFailure::memory: message = "AVIF grid tile 准备内存不足。"; break;
    case TileFailure::length: message = "AVIF grid tile 准备尺寸超过运行时限制。"; break;
    case TileFailure::worker: message = "AVIF grid tile 准备线程异常。"; break;
    case TileFailure::unknown: message = "AVIF grid tile 准备线程异常：未知异常。"; break;
  }
  return awj::compat::unexpected{message};
}

awj::compat::expected<RgbSource, std::string> rgb_source_for_encode(
    std::size_t width,
    std::size_t height,
    std::span<const std::byte> pixels,
    std::size_t stride,
    avifImage* avif_image,
    const NativeEncodeSettings& settings,
    int bit_depth) {
  RgbSource source{};
  const bool keep_alpha = preserve_alpha_for_encode(settings);
  avifRGBImageSetDefaults(&source.rgb, avif_image);
  source.rgb.format = AVIF_RGB_FORMAT_RGBA;
  source.rgb.depth = bit_depth;
  source.rgb.ignoreAlpha = keep_alpha ? AVIF_FALSE : AVIF_TRUE;
  source.rgb.chromaDownsampling = AVIF_CHROMA_DOWNSAMPLING_AVERAGE;
  const auto bytes_per_sample = bit_depth > 8 ? std::size_t{2} : std::size_t{1};
  const auto required_bytes = checked_strided_rgba_bytes(
      width, height, stride, "AVIF encoder", bytes_per_sample);
  if (!required_bytes) {
    return awj::compat::unexpected{required_bytes.error()};
  }
  if (pixels.size() < *required_bytes) {
    return awj::compat::unexpected{"AVIF encoder 输入 RGBA buffer 尺寸无效。"};
  }
  if (bit_depth != 8 && bit_depth != 10 && bit_depth != 12 && bit_depth != 16) {
    return awj::compat::unexpected{"AVIF encoder RGB 输入只支持 8、10、12、16-bit。"};
  }
  if (stride > std::numeric_limits<std::uint32_t>::max()) {
    return awj::compat::unexpected{"AVIF encoder 输入 stride 超出 libavif 限制。"};
  }
  source.rgb.pixels = reinterpret_cast<std::uint8_t*>(
      const_cast<std::byte*>(pixels.data()));
  source.rgb.rowBytes = static_cast<std::uint32_t>(stride);
  return source;
}

awj::compat::expected<RgbSource, std::string> rgb_source_for_encode(
    const ImageBuffer& image,
    const ImagePlane& plane,
    avifImage* avif_image,
    const NativeEncodeSettings& settings) {
  return rgb_source_for_encode(image.width, image.height,
                               plane.read_bytes(),
                               plane.stride, avif_image, settings, image.bit_depth);
}

avifCodecChoice codec_choice_for(AvifEncoderMode) noexcept {
  return AVIF_CODEC_CHOICE_AOM;
}

std::string actual_libavif_id(AvifEncoderMode mode) {
  return avif_encoder_mode_name(mode == AvifEncoderMode::automatic ? AvifEncoderMode::aom : mode);
}

std::string libavif_codec_name_for(AvifEncoderMode mode) {
  return awj::compat::format("libavif-{}", actual_libavif_id(mode));
}

bool libavif_encoder_available(AvifEncoderMode mode) {
  return avifCodecName(codec_choice_for(mode), AVIF_CODEC_FLAG_CAN_ENCODE) != nullptr;
}

bool lossless_requested(const NativeEncodeSettings& settings) noexcept {
  return settings.visual_quality ? *settings.visual_quality >= 100
                                 : settings.quality >= 100;
}

SpeedMapping libavif_speed_mapping(AvifEncoderMode, int speed) {
  speed = std::clamp(speed, 0, 10);
  return SpeedMapping{.user_speed = speed,
                      .codec_value = speed,
                      .codec_key = "aom:cpu-used"};
}

PixelFormat pixel_format_from_avif(avifPixelFormat pixel_format) noexcept {
  switch (pixel_format) {
    case AVIF_PIXEL_FORMAT_YUV444:
      return PixelFormat::yuv444;
    case AVIF_PIXEL_FORMAT_YUV422:
      return PixelFormat::yuv422;
    case AVIF_PIXEL_FORMAT_YUV420:
      return PixelFormat::yuv420;
    case AVIF_PIXEL_FORMAT_YUV400:
      return PixelFormat::gray;
    case AVIF_PIXEL_FORMAT_NONE:
    default:
      return PixelFormat::unknown;
  }
}

std::string avif_error(avifResult result, const avifEncoder* encoder = nullptr) {
  if (encoder != nullptr && encoder->diag.error[0] != '\0') {
    return awj::compat::format("{}: {}", avifResultToString(result), encoder->diag.error);
  }
  return avifResultToString(result);
}

std::string avif_decode_error(avifResult result, const avifDecoder* decoder = nullptr) {
  if (decoder != nullptr && decoder->diag.error[0] != '\0') {
    return awj::compat::format("{}: {}", avifResultToString(result), decoder->diag.error);
  }
  return avifResultToString(result);
}

void configure_decoder(avifDecoder& decoder,
                                         bool copy_metadata_payloads) noexcept {
  // Grid canvases exceed AV1 cell dimensions. Bound allocation by the same
  // runtime byte cap as other decoders, allowing for 16-bit RGBA output.
  decoder.imageSizeLimit = static_cast<std::uint32_t>(std::clamp<std::uint64_t>(
      encoding_defaults::effective_max_input_file_bytes() / 8, 1,
      AVIF_DEFAULT_IMAGE_SIZE_LIMIT));
  decoder.imageDimensionLimit = decoder.imageSizeLimit;
  decoder.imageContentToDecode = AVIF_IMAGE_CONTENT_COLOR_AND_ALPHA | AVIF_IMAGE_CONTENT_GAIN_MAP;
  // Orientation is pixel interpretation, not optional output metadata. ISO
  // reconstruction must see Exif even when the caller will discard metadata.
  decoder.ignoreExif = AVIF_FALSE;
  decoder.ignoreXMP = copy_metadata_payloads ? AVIF_FALSE : AVIF_TRUE;
}

awj::compat::expected<void, std::string> parse_primary_preferred(avifDecoder& decoder) {
  auto result = avifDecoderParse(&decoder);
  if (auto valid = gain_map_detail::validate_avif_iso_gain_map_parse(result, decoder); !valid) return valid;
  if (result != AVIF_RESULT_OK)
    return awj::compat::unexpected{"AVIF 容器解析失败: " + avif_decode_error(result, &decoder)};
  const int primary = awjAvifHasIndependentPrimary(&decoder);
  if (primary < 0) return awj::compat::unexpected{"AVIF 主图与序列的关联无法安全确认，拒绝以动画替代静态主图。"};
  if (decoder.imageSequenceTrackPresent) {
    const auto source = primary ? AVIF_DECODER_SOURCE_PRIMARY_ITEM : AVIF_DECODER_SOURCE_TRACKS;
    result = avifDecoderSetSource(&decoder, source);
    if (result == AVIF_RESULT_OK) result = avifDecoderParse(&decoder);
    if (auto valid = gain_map_detail::validate_avif_iso_gain_map_parse(result, decoder); !valid) return valid;
    if (result != AVIF_RESULT_OK)
      return awj::compat::unexpected{"AVIF 静态主图/序列选择失败: " + avif_decode_error(result, &decoder)};
  }
  return {};
}

}  // namespace avif_aom_detail

bool avif_libavif_encoder_available(AvifEncoderMode mode) {
  return (mode == AvifEncoderMode::aom || mode == AvifEncoderMode::automatic) &&
         avif_aom_detail::libavif_encoder_available(mode);
}

bool avif_dav1d_decoder_available() noexcept {
  return avifCodecName(AVIF_CODEC_CHOICE_DAV1D, AVIF_CODEC_FLAG_CAN_DECODE) != nullptr;
}

std::vector<AvifEncoderCapability> avif_encoder_capabilities_for_current_build() {
  return avif_encoder_capabilities_for_build(avif_libavif_encoder_available(AvifEncoderMode::aom));
}

awj::compat::expected<AvifEncoderSelection, std::string> select_avif_encoder_for_current_build(
    const AvifEncoderSelectionRequest& request) {
  const auto capabilities = avif_encoder_capabilities_for_current_build();
  return select_avif_encoder_from_capabilities(request, capabilities);
}

awj::compat::expected<NativeEncodeResult, std::string> encode_with_current_settings(
    const ImageBuffer& source_image,
    const NativeEncodeSettings& settings,
    awj::compat::stop_token stop_token) {
  if (auto stopped = avif_aom_detail::stop_if_requested(stop_token); !stopped) {
    return awj::compat::unexpected{stopped.error()};
  }
  if (auto valid = avif_aom_detail::validate_avif_color_settings(settings); !valid) {
    return awj::compat::unexpected{valid.error()};
  }
  const bool lossless = avif_aom_detail::lossless_requested(settings);
  const auto actual_mode = AvifEncoderMode::aom;
  auto applied_chroma = avif_aom_detail::applied_chroma_from_settings(
      source_image, settings.chroma_mode);
  if (settings.avif_grid_plan) {
    const auto& plan = *settings.avif_grid_plan;
    const bool horizontal_misaligned =
        (source_image.width & 1u) != 0 || (plan.tile_width & 1u) != 0;
    const bool vertical_misaligned =
        (source_image.height & 1u) != 0 || (plan.tile_height & 1u) != 0;
    const bool incompatible =
        (applied_chroma == ChromaMode::yuv422 && horizontal_misaligned) ||
        (applied_chroma == ChromaMode::yuv420 &&
         (horizontal_misaligned || vertical_misaligned));
    if (incompatible) {
      return awj::compat::unexpected{
          "AVIF grid 的 420/422 色度与当前奇数输出或 cell 尺寸不兼容；请使用 --chroma 444。"};
    }
  }
  const auto pixel_format =
      avif_aom_detail::avif_pixel_format_from_chroma(applied_chroma);
  if (!pixel_format) {
    return awj::compat::unexpected{pixel_format.error()};
  }

  if (auto valid = validate_image_pixels(source_image); !valid) return awj::compat::unexpected{valid.error()};
  if (source_image.width > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) ||
      source_image.height > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) ||
      source_image.planes.front().stride > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
    return awj::compat::unexpected{"AVIF encoder 输入尺寸超过 libavif API 限制。"};
  }
  if (!settings.avif_grid_plan &&
      (source_image.width > static_cast<std::size_t>(encoding_defaults::avif_single_image_max_dimension) ||
       source_image.height > static_cast<std::size_t>(encoding_defaults::avif_single_image_max_dimension))) {
    return awj::compat::unexpected{awj::compat::format(
        "AVIF 单图编码输入尺寸 {}x{} 超过边长上限 {}；超过单图上限时应走自动大图链路（AOM Grid）。",
        source_image.width, source_image.height, encoding_defaults::avif_single_image_max_dimension)};
  }
  if (!settings.avif_grid_plan &&
      static_cast<std::uint64_t>(source_image.width) *
              static_cast<std::uint64_t>(source_image.height) >
          encoding_defaults::avif_single_image_max_pixels) {
    return awj::compat::unexpected{awj::compat::format(
        "AVIF 单图编码输入像素数 {} 超过上限 {}；超过单图上限时应走自动大图链路（AOM Grid）。",
        static_cast<std::uint64_t>(source_image.width) *
            static_cast<std::uint64_t>(source_image.height),
        encoding_defaults::avif_single_image_max_pixels)};
  }
  if (lossless && !settings.bit_depth && source_image.source_info &&
      source_image.source_info->bit_depth > 0 && source_image.source_info->bit_depth != 8 &&
      source_image.source_info->bit_depth != 10 && source_image.source_info->bit_depth != 12) {
    return awj::compat::unexpected{awj::compat::format(
        "AVIF 无损模式无法保持源图 {}-bit 位深；libavif AOM 当前仅支持 8、10、12-bit 输出。",
        source_image.source_info->bit_depth)};
  }
  const int bit_depth = avif_aom_detail::applied_bit_depth_from_settings(
      source_image, settings, lossless);
  if (bit_depth != 8 && bit_depth != 10 && bit_depth != 12) {
    return awj::compat::unexpected{"AVIF encoder 只支持 8、10、12-bit 输出。"};
  }

  IsoGainMapPtr gain_map{nullptr, &avifGainMapDestroy};
  EncodeTimingDiagnostics gain_map_timing{};
  if (settings.gain_map_alternative) {
    // Grid 使用全尺寸 map，cell 边界与主图严格一一对应，避免比例舍入及不同 cell 元数据漂移。
    const auto width = settings.avif_grid_plan ? static_cast<std::uint32_t>(source_image.width) : 0u;
    const auto height = settings.avif_grid_plan ? static_cast<std::uint32_t>(source_image.height) : 0u;
    auto computed = compute_iso_gain_map(source_image, *settings.gain_map_alternative,
        width, height, 8, stop_token, settings.resources, &gain_map_timing);
    if (!computed) return awj::compat::unexpected{computed.error()};
    gain_map = std::move(*computed);
  }

  const auto gain_map_width = gain_map && gain_map->image ? gain_map->image->width : 0u;
  const auto gain_map_height = gain_map && gain_map->image ? gain_map->image->height : 0u;
  avifImage target_description{};
  avif_aom_detail::apply_color_settings(target_description, settings, applied_chroma, lossless);
  const PixelFormat target_format = *pixel_format == AVIF_PIXEL_FORMAT_YUV444 ? PixelFormat::yuv444
      : *pixel_format == AVIF_PIXEL_FORMAT_YUV422 ? PixelFormat::yuv422
      : *pixel_format == AVIF_PIXEL_FORMAT_YUV420 ? PixelFormat::yuv420 : PixelFormat::gray;
  const PixelConversionRequest conversion{.format = target_format, .bit_depth = bit_depth,
      .matrix_coefficients = static_cast<int>(target_description.matrixCoefficients),
      .color_range = target_description.yuvRange == AVIF_RANGE_FULL ? 1 : 0,
      .preserve_alpha = avif_aom_detail::preserve_alpha_for_encode(settings)};
  std::optional<ImageBuffer> converted_pixels;
  const auto conversion_started = avif_aom_detail::Clock::now();
  if (!pixels_match(source_image, conversion)) {
    auto converted = convert_image_pixels(source_image, conversion, stop_token, settings.resources);
    if (!converted) return awj::compat::unexpected{converted.error()};
    converted_pixels = std::move(*converted);
  }
  const double conversion_seconds = avif_aom_detail::elapsed_seconds(conversion_started);
  const ImageBuffer& image = converted_pixels ? *converted_pixels : source_image;
  const ImagePlane* input_plane = &image.planes.front();

  avif_aom_detail::AvifEncoder encoder{avifEncoderCreate()};
  if (!encoder) {
    return awj::compat::unexpected{"无法创建 libavif encoder。"};
  }
  encoder->codecChoice = avif_aom_detail::codec_choice_for(actual_mode);
  const int final_quality = lossless ? AVIF_QUALITY_LOSSLESS : std::clamp(settings.quality, 1, 100);
  encoder->quality = final_quality;
  encoder->qualityAlpha = final_quality;
  encoder->qualityGainMap = final_quality;
  const int encoder_speed = std::clamp(settings.speed, 0, 10);
  encoder->speed = encoder_speed;
  encoder->keyframeInterval = 1;
  const int total_encoder_threads = avif_aom_detail::codec_thread_count(
      settings.resources.encoder_threads_per_file);
  encoder->maxThreads = total_encoder_threads;

  const auto set_option = [&](std::string_view key, std::string_view value) -> awj::compat::expected<void, std::string> {
    const avifResult option_result = avifEncoderSetCodecSpecificOption(
        encoder.get(), std::string{key}.c_str(), std::string{value}.c_str());
    if (option_result != AVIF_RESULT_OK) {
      return awj::compat::unexpected{awj::compat::format("AVIF AOM 设置参数 {}={} 失败: {}",
                                         key, value,
                                         avif_aom_detail::avif_error(option_result, encoder.get()))};
    }
    return {};
  };
  if (auto set = set_option("row-mt", total_encoder_threads > 1 ? "1" : "0"); !set) {
    return awj::compat::unexpected{set.error()};
  }
  if (!lossless && settings.avif_tune_iq) {
    if (auto set = set_option("color:tune", "iq"); !set) {
      return awj::compat::unexpected{set.error()};
    }
  }

  auto output_holder = avif_aom_detail::make_avif_rw_data();
  if (!output_holder) {
    return awj::compat::unexpected{output_holder.error()};
  }
  auto output = std::move(*output_holder);

  avifResult result = AVIF_RESULT_OK;
  double rgb_to_yuv_seconds = conversion_seconds;
  double add_image_seconds = -1.0;
  double finish_seconds = -1.0;
  double output_copy_seconds = -1.0;
  if (settings.avif_grid_plan) {
    const auto& plan = *settings.avif_grid_plan;
    if (plan.cols == 0 || plan.rows == 0 || plan.tile_width == 0 || plan.tile_height == 0) {
      return awj::compat::unexpected{"AVIF grid 规划无效。"};
    }
    if (plan.tile_width > encoding_defaults::avif_single_image_max_dimension ||
        plan.tile_height > encoding_defaults::avif_single_image_max_dimension) {
      return awj::compat::unexpected{awj::compat::format(
          "AVIF grid tile 尺寸 {}x{} 超过单图边长上限 {}。",
          plan.tile_width, plan.tile_height, encoding_defaults::avif_single_image_max_dimension)};
    }
    const auto planned_width = static_cast<std::uint64_t>(plan.cols) * plan.tile_width;
    const auto planned_height = static_cast<std::uint64_t>(plan.rows) * plan.tile_height;
    if (planned_width < static_cast<std::uint64_t>(image.width) ||
        planned_height < static_cast<std::uint64_t>(image.height) ||
        planned_width - plan.tile_width >= static_cast<std::uint64_t>(image.width) ||
        planned_height - plan.tile_height >= static_cast<std::uint64_t>(image.height)) {
      return awj::compat::unexpected{"AVIF grid 规划尺寸与输入图片不一致。"};
    }
    const auto tile_count = static_cast<std::uint64_t>(plan.cols) * plan.rows;
    if (tile_count == 0 || tile_count > std::numeric_limits<std::size_t>::max()) {
      return awj::compat::unexpected{"AVIF grid tile 数量过大。"};
    }
    const auto grid_resources = plan_grid_encode_resources(
        settings.resources,
        static_cast<int>(std::min<std::uint64_t>(
            tile_count, static_cast<std::uint64_t>(std::numeric_limits<int>::max()))));
    encoder->maxThreads = total_encoder_threads;

    std::vector<avif_aom_detail::AvifImage> tile_storage;
    std::vector<const avifImage*> tile_views;
    try {
      tile_storage.resize(static_cast<std::size_t>(tile_count));
      tile_views.resize(static_cast<std::size_t>(tile_count));
    } catch (const std::bad_alloc&) {
      return awj::compat::unexpected{"AVIF grid tile 列表内存不足。"};
    } catch (const std::length_error&) {
      return awj::compat::unexpected{"AVIF grid tile 列表尺寸超过运行时限制。"};
    }

    const avif_aom_detail::GridTileContext tile_context{
        .image = &image,
        .plane = input_plane,
        .settings = &settings,
        .plan = &plan,
        .pixel_format = *pixel_format,
        .applied_chroma = applied_chroma,
        .lossless = lossless,
        .bit_depth = bit_depth,
        .gain_map = gain_map.get()};
    const int tile_prepare_threads =
        avif_aom_detail::grid_tile_prepare_thread_count(tile_storage.size(),
                                                        grid_resources);
    auto prepared_tiles = avif_aom_detail::prepare_grid_tiles_parallel(
        tile_context, tile_storage, tile_views, tile_prepare_threads, stop_token);
    if (!prepared_tiles) {
      return awj::compat::unexpected{prepared_tiles.error()};
    }

    if (auto stopped = avif_aom_detail::stop_if_requested(stop_token); !stopped) {
      return awj::compat::unexpected{stopped.error()};
    }
    auto substage_started = avif_aom_detail::Clock::now();
    result = avifEncoderAddImageGrid(
        encoder.get(), plan.cols, plan.rows,
        reinterpret_cast<const avifImage* const*>(tile_views.data()),
        AVIF_ADD_IMAGE_FLAG_SINGLE);
    add_image_seconds =
        avif_aom_detail::elapsed_seconds(substage_started);
    if (auto stopped = avif_aom_detail::stop_if_requested(stop_token); !stopped) {
      return awj::compat::unexpected{stopped.error()};
    }
    if (result != AVIF_RESULT_OK) {
      return awj::compat::unexpected{awj::compat::format("AVIF grid 编码失败: {}",
                                         avif_aom_detail::avif_error(result, encoder.get()))};
    }
    if (auto stopped = avif_aom_detail::stop_if_requested(stop_token); !stopped) {
      return awj::compat::unexpected{stopped.error()};
    }
    substage_started = avif_aom_detail::Clock::now();
    result = avifEncoderFinish(encoder.get(), output.get());
    finish_seconds = avif_aom_detail::elapsed_seconds(substage_started);
    if (auto stopped = avif_aom_detail::stop_if_requested(stop_token); !stopped) {
      return awj::compat::unexpected{stopped.error()};
    }
  } else {
    avif_aom_detail::AvifImage avif_image{avifImageCreate(
        static_cast<std::uint32_t>(image.width),
        static_cast<std::uint32_t>(image.height), bit_depth, *pixel_format)};
    if (!avif_image) {
      return awj::compat::unexpected{"无法创建 libavif image。"};
    }
    avif_aom_detail::apply_color_settings(*avif_image, settings, applied_chroma, lossless);
    if (auto metadata = avif_aom_detail::apply_avif_metadata(*avif_image, image, settings); !metadata) {
      return awj::compat::unexpected{metadata.error()};
    }

    auto planes = yuv_plane_view(image);
    if (!planes) return awj::compat::unexpected{planes.error()};
    for (int c = 0; c < 3; ++c) {
      avif_image->yuvPlanes[c] = planes->yuvPlanes[c];
      avif_image->yuvRowBytes[c] = planes->yuvRowBytes[c];
    }
    avif_image->alphaPlane = planes->alphaPlane;
    avif_image->alphaRowBytes = planes->alphaRowBytes;
    avif_image->alphaPremultiplied = AVIF_FALSE;
    if (gain_map) {
      gain_map->altPlaneCount = 3;
      avif_image->gainMap = gain_map.release();
    }
    auto substage_started = avif_aom_detail::Clock::now();
    result = AVIF_RESULT_OK;
    if (auto stopped = avif_aom_detail::stop_if_requested(stop_token); !stopped) {
      return awj::compat::unexpected{stopped.error()};
    }
    if (result != AVIF_RESULT_OK) {
      return awj::compat::unexpected{awj::compat::format("AVIF RGB 转 YUV 失败: {}",
                                         avifResultToString(result))};
    }
    if (auto stopped = avif_aom_detail::stop_if_requested(stop_token); !stopped) {
      return awj::compat::unexpected{stopped.error()};
    }
    substage_started = avif_aom_detail::Clock::now();
    result = avifEncoderAddImage(encoder.get(), avif_image.get(), 1,
                                 AVIF_ADD_IMAGE_FLAG_SINGLE);
    add_image_seconds =
        avif_aom_detail::elapsed_seconds(substage_started);
    if (auto stopped = avif_aom_detail::stop_if_requested(stop_token); !stopped) {
      return awj::compat::unexpected{stopped.error()};
    }
    if (result == AVIF_RESULT_OK) {
      substage_started = avif_aom_detail::Clock::now();
      result = avifEncoderFinish(encoder.get(), output.get());
      finish_seconds =
          avif_aom_detail::elapsed_seconds(substage_started);
      if (auto stopped = avif_aom_detail::stop_if_requested(stop_token);
          !stopped) {
        return awj::compat::unexpected{stopped.error()};
      }
    }
  }
  if (result != AVIF_RESULT_OK) {
    return awj::compat::unexpected{awj::compat::format("AVIF {} 编码失败: {}",
                                       avif_encoder_mode_name(actual_mode),
                                       avif_aom_detail::avif_error(result, encoder.get()))};
  }
  if (output->size == 0 || output->data == nullptr) {
    return awj::compat::unexpected{"AVIF 编码输出为空。"};
  }
  if (output->size > encoding_defaults::effective_max_input_file_bytes()) {
    return awj::compat::unexpected{"AVIF 编码输出超过当前运行时上限。"};
  }

  if (auto stopped = avif_aom_detail::stop_if_requested(stop_token); !stopped) {
    return awj::compat::unexpected{stopped.error()};
  }

  EncodedImage encoded{.codec_name = avif_aom_detail::libavif_codec_name_for(actual_mode)};
  auto encoded_bytes = decoder_common::make_byte_buffer(output->size, "AVIF encoder");
  if (!encoded_bytes) {
    return awj::compat::unexpected{encoded_bytes.error()};
  }
  encoded.bytes = std::move(*encoded_bytes);
  if (auto stopped = avif_aom_detail::stop_if_requested(stop_token); !stopped) {
    return awj::compat::unexpected{stopped.error()};
  }
  const auto copy_started = avif_aom_detail::Clock::now();
  awj::compat::ranges::copy_n(reinterpret_cast<std::byte*>(output->data), output->size,
                      encoded.bytes.begin());
  output_copy_seconds = avif_aom_detail::elapsed_seconds(copy_started);

  auto diagnostics = diagnostics_from_settings(settings);
  diagnostics.encoder_id = avif_encoder_mode_name(actual_mode);
  diagnostics.requested_encoder_id = avif_encoder_mode_name(settings.requested_avif_encoder);
  diagnostics.requested_chroma = chroma_mode_name(settings.requested_chroma_mode);
  diagnostics.applied_chroma = chroma_mode_name(applied_chroma);
  diagnostics.requested_bit_depth = settings.requested_bit_depth;
  diagnostics.applied_bit_depth = bit_depth;
  diagnostics.bit_depth_reason = settings.bit_depth_reason.empty()
                                     ? avif_aom_detail::default_bit_depth_reason(
                                           image, settings, lossless)
                                     : settings.bit_depth_reason;
  diagnostics.fallback_reason = settings.encoder_fallback_reason;
  diagnostics.encoder_license = "BSD-2-Clause";
  diagnostics.integration_mode = settings.gain_map_alternative
      ? (settings.avif_grid_plan ? "libavif-grid-iso21496-1" : "libavif-iso21496-1")
      : settings.avif_grid_plan ? "libavif-grid" : std::string{};
  if (settings.gain_map_alternative) {
    diagnostics.applied_hdr_metadata = "iso21496-1-gain-map-hdr-base";
    diagnostics.color_reason += "；保留 HDR 主图与原 SDR/低端呈现，重新编码 ISO 21496-1 Gain Map；整对不宣称逐样本无损";
  }
  diagnostics.speed_mapping = avif_aom_detail::libavif_speed_mapping(actual_mode, encoder_speed);
  diagnostics.encoder_threads = total_encoder_threads;
  diagnostics.memory_budget_bytes = settings.resources.memory_limit_bytes;
  diagnostics.timing.avif_rgb_to_yuv_seconds = rgb_to_yuv_seconds;
  diagnostics.timing.avif_add_image_seconds = add_image_seconds;
  diagnostics.timing.avif_finish_seconds = finish_seconds;
  diagnostics.timing.avif_output_copy_seconds = output_copy_seconds;
  diagnostics.timing.gain_map_compute_seconds = gain_map_timing.gain_map_compute_seconds;
  diagnostics.timing.gain_map_compute_workers = gain_map_timing.gain_map_compute_workers;
  diagnostics.rendition.gain_map_generated = settings.gain_map_alternative != nullptr;
  diagnostics.rendition.gain_map_width = gain_map_width;
  diagnostics.rendition.gain_map_height = gain_map_height;
  diagnostics.rendition.gain_map_bit_depth = gain_map_width ? 8 : 0;

  return NativeEncodeResult{.encoded = std::move(encoded),
                            .diagnostics = std::move(diagnostics),
                            .final_quality = final_quality,
                            .lossless = lossless && !settings.gain_map_alternative &&
                                bit_depth >= rendition_reference_depth(source_image),
                            .search_attempt_count = 1};
}

awj::compat::expected<NativeEncodeResult, std::string> encode_avif_animation(
    AnimationReader& reader, const NativeEncodeSettings& settings, awj::compat::stop_token stop_token) {
  // Caller has advanced to the first full-canvas frame to prepare color/depth settings.
  const auto& info = reader.info();
  if (info.durations.empty() || settings.avif_grid_plan)
    return awj::compat::unexpected{"动画 AVIF 必须有帧，且不能使用静态 Grid。"};
  if (info.width > encoding_defaults::avif_single_image_max_dimension ||
      info.height > encoding_defaults::avif_single_image_max_dimension ||
      static_cast<std::uint64_t>(info.width) * info.height > encoding_defaults::avif_single_image_max_pixels)
    return awj::compat::unexpected{"动画 AVIF 画布超过 AV1 单帧限制；请使用尺寸限制缩小画布。"};
  if (auto valid = avif_aom_detail::validate_avif_color_settings(settings); !valid)
    return awj::compat::unexpected{valid.error()};
  auto applied_settings = settings;
  applied_settings.source_has_alpha_channel = true;
  applied_settings.encoder_supports_alpha = true;
  applied_settings.applied_alpha = settings.alpha_policy == AlphaModePolicy::off ? "dropped" : "kept";
  const bool lossless = avif_aom_detail::lossless_requested(settings);
  const auto chroma = avif_aom_detail::applied_chroma_from_settings(reader.frame(), settings.chroma_mode);
  auto format = avif_aom_detail::avif_pixel_format_from_chroma(chroma);
  if (!format) return awj::compat::unexpected{format.error()};
  const int depth = avif_aom_detail::applied_bit_depth_from_settings(reader.frame(), settings, lossless);
  if (depth != 8 && depth != 10 && depth != 12) return awj::compat::unexpected{"动画 AVIF 位深必须为 8/10/12。"};
  avif_aom_detail::AvifEncoder encoder{avifEncoderCreate()};
  if (!encoder) return awj::compat::unexpected{"无法创建动画 AVIF encoder。"};
  encoder->codecChoice = AVIF_CODEC_CHOICE_AOM;
  encoder->quality = lossless ? AVIF_QUALITY_LOSSLESS : std::clamp(settings.quality, 1, 100);
  encoder->qualityAlpha = encoder->quality;
  encoder->speed = std::clamp(settings.speed, 0, 10);
  encoder->maxThreads = avif_aom_detail::codec_thread_count(settings.resources.encoder_threads_per_file);
  encoder->keyframeInterval = settings.avif_animation_keyframe;
  encoder->timescale = animation_timescale(info.durations);
  encoder->repetitionCount = info.repetitions;
  if (!lossless && settings.avif_animation_tune != AvifAnimationTune::automatic) {
    const auto option = avifEncoderSetCodecSpecificOption(encoder.get(), "color:tune",
        settings.avif_animation_tune == AvifAnimationTune::ssim ? "ssim" : "psnr");
    if (option != AVIF_RESULT_OK) return awj::compat::unexpected{avif_aom_detail::avif_error(option, encoder.get())};
  }
  const auto budget = settings.resources.memory_limit_bytes;
  const auto raw_reserve = static_cast<std::uint64_t>(info.width) * info.height * 32;
  auto check_budget = [&](std::size_t frame_index) -> awj::compat::expected<void, std::string> {
    if (!budget) return {};
    const auto retained = awjAvifEncoderRetainedBytes(encoder.get());
    const auto reader_bytes = reader.memory_usage_bytes();
    const auto overhead = static_cast<std::uint64_t>(frame_index + 1) * 4096;
    if (reader_bytes > budget || raw_reserve > budget - reader_bytes ||
        overhead > budget - reader_bytes - raw_reserve ||
        retained > (budget - reader_bytes - raw_reserve - overhead) / 3)
      return awj::compat::unexpected{"动画 AVIF 累积压缩数据与帧工作区超出内存预算。"};
    return {};
  };
  long double seconds = 0, compensation = 0;
  std::uint64_t previous_ticks = 0;
  for (std::size_t index = 0; index < info.durations.size(); ++index) {
    if (stop_token.stop_requested()) return awj::compat::unexpected{"任务已取消。"};
    if (auto fits = check_budget(index); !fits) return awj::compat::unexpected{fits.error()};
    {
    const auto& frame = reader.frame();
    if (frame.width != info.width || frame.height != info.height)
      return awj::compat::unexpected{"动画帧画布尺寸不一致。"};
    auto plane = avif_aom_detail::rgba_plane(frame, "动画 AVIF");
    if (!plane) return awj::compat::unexpected{plane.error()};
    avif_aom_detail::AvifImage image{avifImageCreate(static_cast<std::uint32_t>(info.width),
        static_cast<std::uint32_t>(info.height), depth, *format)};
    if (!image) return awj::compat::unexpected{"无法创建动画 AVIF 帧。"};
    avif_aom_detail::apply_color_settings(*image, applied_settings, chroma, lossless);
    auto metadata = avif_aom_detail::apply_avif_metadata(*image, frame, applied_settings);
    if (!metadata) return awj::compat::unexpected{metadata.error()};
    auto rgb = avif_aom_detail::rgb_source_for_encode(frame, **plane, image.get(), applied_settings);
    if (!rgb) return awj::compat::unexpected{rgb.error()};
    auto result = avifImageRGBToYUV(image.get(), &rgb->rgb);
    if (result != AVIF_RESULT_OK) return awj::compat::unexpected{avif_aom_detail::avif_error(result, encoder.get())};
    const auto duration = info.durations[index];
    if (!duration.numerator || !duration.denominator) return awj::compat::unexpected{"动画帧时长无效。"};
    // Cumulative rounding avoids accumulating one quantization error per VFR frame.
    const auto increment = static_cast<long double>(duration.numerator) / duration.denominator - compensation;
    const auto next_seconds = seconds + increment;
    compensation = (next_seconds - seconds) - increment;
    seconds = next_seconds;
    const auto ticks = std::floor(seconds * encoder->timescale + 0.5L);
    if (!std::isfinite(ticks) || ticks > static_cast<long double>(std::numeric_limits<std::int64_t>::max()) ||
        ticks <= previous_ticks) return awj::compat::unexpected{"动画时长量化溢出或小于一个 tick。"};
    const auto total_ticks = static_cast<std::uint64_t>(ticks);
    if (total_ticks - previous_ticks > std::numeric_limits<std::uint32_t>::max())
      return awj::compat::unexpected{"动画帧时长超过 AVIF 的 32 位 sample_delta 限制。"};
    const auto flags = info.durations.size() == 1 ? AVIF_ADD_IMAGE_FLAG_SINGLE : AVIF_ADD_IMAGE_FLAG_NONE;
    result = avifEncoderAddImage(encoder.get(), image.get(), total_ticks - previous_ticks, flags);
    if (result != AVIF_RESULT_OK) return awj::compat::unexpected{avif_aom_detail::avif_error(result, encoder.get())};
    previous_ticks = total_ticks;
    } // Release AVIF/RGB workspaces before decoding the next rectangle.
    if (stop_token.stop_requested()) return awj::compat::unexpected{"任务已取消。"};
    if (auto fits = check_budget(index); !fits) return awj::compat::unexpected{fits.error()};
    if (settings.frame_progress) settings.frame_progress(index + 1, info.durations.size());
    if (index + 1 < info.durations.size()) {
      auto next = reader.next(stop_token);
      if (!next) return awj::compat::unexpected{next.error()};
      if (!*next) return awj::compat::unexpected{"动画帧数据提前结束。"};
    }
  }
  if (stop_token.stop_requested()) return awj::compat::unexpected{"任务已取消。"};
  auto output_holder = avif_aom_detail::make_avif_rw_data();
  if (!output_holder) return awj::compat::unexpected{output_holder.error()};
  auto output = std::move(*output_holder);
  const auto result = avifEncoderFinish(encoder.get(), output.get());
  if (result != AVIF_RESULT_OK) return awj::compat::unexpected{avif_aom_detail::avif_error(result, encoder.get())};
  if (stop_token.stop_requested()) return awj::compat::unexpected{"任务已取消。"};
  if (output->size > encoding_defaults::effective_max_input_file_bytes() || (budget && output->size > budget / 3))
    return awj::compat::unexpected{"动画 AVIF 输出超过内存预算。"};
  auto bytes = decoder_common::make_byte_buffer(output->size, "动画 AVIF");
  if (!bytes) return awj::compat::unexpected{bytes.error()};
  std::memcpy(bytes->data(), output->data, output->size);
  auto diagnostics = diagnostics_from_settings(applied_settings);
  diagnostics.encoder_id = "aom";
  diagnostics.integration_mode = "libavif-sequence";
  diagnostics.applied_bit_depth = depth;
  diagnostics.applied_chroma = chroma_mode_name(chroma);
  diagnostics.encoder_threads = encoder->maxThreads;
  return NativeEncodeResult{.encoded = {.bytes = std::move(*bytes), .codec_name = "libavif-sequence"},
      .diagnostics = std::move(diagnostics), .final_quality = encoder->quality,
      .lossless = lossless, .search_attempt_count = 1};
}

class AvifLibavifImageEncoder final : public ImageEncoder {
 public:
  explicit AvifLibavifImageEncoder(AvifEncoderMode mode) : mode_{mode} {}

  [[nodiscard]] std::string_view id() const noexcept override {
    return "aom";
  }

  [[nodiscard]] CodecCapabilities capabilities() const override {
    return CodecCapabilities{.output_format = OutputFormat::avif,
                             .features = CodecFeature::alpha |
                                         CodecFeature::thread_control,
                             .min_quality = 1,
                             .max_quality = 100,
                             .min_speed = 0,
                             .max_speed = 10,
                             .bit_depths = {8, 10, 12}};
  }

  awj::compat::expected<NativeEncodeResult, std::string> encode(
      const ImageBuffer& image,
      const NativeEncodeSettings& settings,
      awj::compat::stop_token stop_token = {}) const override {
    try {
      if (auto stopped = avif_aom_detail::stop_if_requested(stop_token); !stopped) {
        return awj::compat::unexpected{stopped.error()};
      }
      if (!avif_aom_detail::libavif_encoder_available(mode_)) {
        return awj::compat::unexpected{awj::compat::format(
            "AVIF encoder {} is not available in this libavif build.",
            avif_encoder_mode_name(mode_))};
      }
      auto effective_settings = settings;
      if (!effective_settings.speed_explicit) {
        effective_settings.speed = default_speed_for(OutputFormat::avif);
      }
      return encode_with_current_settings(image, effective_settings, stop_token);
    } catch (const std::bad_alloc&) {
      return awj::compat::unexpected{"AVIF 编码内存不足。"};
    } catch (const std::length_error&) {
      return awj::compat::unexpected{"AVIF 编码数据超过运行时限制。"};
    } catch (const std::filesystem::filesystem_error&) {
      return awj::compat::unexpected{"AVIF 编码文件系统访问失败。"};
    }
  }

 private:
  AvifEncoderMode mode_{};
};

class AvifImageDecoder final : public ImageDecoder {
 public:
  explicit AvifImageDecoder(int decode_threads = 1)
      : decode_threads_{avif_aom_detail::codec_thread_count(decode_threads)} {}

  [[nodiscard]] std::string_view id() const noexcept override { return "libavif"; }

  [[nodiscard]] bool can_decode(const fs::path& path) const override {
    auto ext = path.extension().wstring();
    awj::compat::ranges::transform(ext, ext.begin(),
                           [](wchar_t ch) { return std::towlower(ch); });
    return ext == L".avif" || ext == L".avifs";
  }

  awj::compat::expected<ImageDimensions, std::string> probe_dimensions(
      const fs::path& path) const override {
    try {
      auto file_io = avif_aom_detail::make_avif_file_io(path);
      if (!file_io) {
        return awj::compat::unexpected{file_io.error()};
      }
      avif_aom_detail::AvifDecoder decoder{avifDecoderCreate()};
      if (!decoder) {
        return awj::compat::unexpected{"无法创建 libavif decoder。"};
      }
      decoder->codecChoice = AVIF_CODEC_CHOICE_AUTO;
      decoder->maxThreads = 1;
      avif_aom_detail::configure_decoder(*decoder, false);
      avifDecoderSetIO(decoder.get(), &(*file_io)->io);
      if (auto parsed = avif_aom_detail::parse_primary_preferred(*decoder); !parsed)
        return awj::compat::unexpected{parsed.error()};
      if (decoder->image == nullptr) {
        return awj::compat::unexpected{awj::compat::format("AVIF 图像信息为空: {}", display_path_for_user(path))};
      }
      ImageBuffer dimensions{.width = decoder->image->width, .height = decoder->image->height};
      if (auto geometry = avif_aom_detail::apply_display_geometry(dimensions, *decoder->image); !geometry)
        return awj::compat::unexpected{decoder->image->gainMap ? "Gain Map: ISO 21496-1 AVIF: " + geometry.error() : geometry.error()};
      if (decoder->image->gainMap && !(decoder->image->transformFlags & (AVIF_TRANSFORM_IROT | AVIF_TRANSFORM_IMIR))) {
        std::size_t offset{};
        const auto& exif = decoder->image->exif;
        if (exif.data && exif.size && avifGetExifOrientationOffset(exif.data, exif.size, &offset) == AVIF_RESULT_OK && offset < exif.size) {
          const auto orientation = exif.data[offset];
          if (orientation < 1 || orientation > 8) return awj::compat::unexpected{"Gain Map: ISO 21496-1 AVIF: Exif orientation 无效。"};
          if (orientation >= 5) std::swap(dimensions.width, dimensions.height);
        }
      }
      auto profile = decoder_common::make_image_dimensions_checked(dimensions.width, dimensions.height, "AVIF");
      if (!profile) return profile;
      const auto& image = *decoder->image;
      profile->source_bit_depth = static_cast<int>(image.depth);
      profile->source_has_alpha = decoder->alphaPresent != AVIF_FALSE;
      profile->source_has_gain_map = image.gainMap != nullptr;
      profile->source_icc_bytes = std::max<std::uint64_t>(image.icc.size,
          image.gainMap ? image.gainMap->altICC.size : 0);
      const bool hdr = avif_aom_detail::has_hdr_metadata(image);
      profile->source_dynamic_range = hdr ? ImageDynamicRangeHint::hdr : !image.icc.size &&
          image.transferCharacteristics != 0 && image.transferCharacteristics != AVIF_TRANSFER_CHARACTERISTICS_UNSPECIFIED
          ? ImageDynamicRangeHint::sdr : ImageDynamicRangeHint::unknown;
      if (image.gainMap) {
        profile->source_bit_depth = 16;
        profile->source_dynamic_range = hdr || image.gainMap->baseHdrHeadroom.n || image.gainMap->alternateHdrHeadroom.n
            ? ImageDynamicRangeHint::hdr : ImageDynamicRangeHint::sdr;
        if (image.gainMap->image) {
          profile->gain_map_width = image.gainMap->image->width;
          profile->gain_map_height = image.gainMap->image->height;
        }
      }
      profile->opaque_sdr8_yuv420 = !image.gainMap && !image.icc.size && !profile->source_has_alpha &&
          image.depth == 8 && image.yuvFormat == AVIF_PIXEL_FORMAT_YUV420 &&
          profile->source_dynamic_range == ImageDynamicRangeHint::sdr;
      const auto pixels = static_cast<std::uint64_t>(image.width) * image.height;
      const auto bytes_per_pixel = profile->source_bit_depth > 8 ? 8u : 4u;
      profile->decoded_storage_bytes = pixels > UINT64_MAX / bytes_per_pixel ? UINT64_MAX : pixels * bytes_per_pixel;
      return profile;
    } catch (const std::bad_alloc&) {
      return awj::compat::unexpected{"AVIF 尺寸探测内存不足。"};
    } catch (const std::length_error&) {
      return awj::compat::unexpected{"AVIF 尺寸探测数据超过运行时限制。"};
    } catch (const std::filesystem::filesystem_error&) {
      return awj::compat::unexpected{"AVIF 尺寸探测文件系统访问失败。"};
    }
  }

  awj::compat::expected<ImageBuffer, std::string> parse_container_info(
      const fs::path& path) const {
    try {
      auto file_io = avif_aom_detail::make_avif_file_io(path);
      if (!file_io) {
        return awj::compat::unexpected{file_io.error()};
      }
      return parse_container_file(**file_io, display_path_for_user(path), false);
    } catch (const std::bad_alloc&) {
      return awj::compat::unexpected{"AVIF 容器信息读取内存不足。"};
    } catch (const std::length_error&) {
      return awj::compat::unexpected{"AVIF 容器信息读取数据超过运行时限制。"};
    } catch (const std::filesystem::filesystem_error&) {
      return awj::compat::unexpected{"AVIF 容器信息读取文件系统访问失败。"};
    }
  }

  awj::compat::expected<ImageDecodeResult, std::string> decode_memory(
      std::span<const std::byte> bytes,
      std::string_view source_name,
      DecodeOptions options = {}) const override {
    return decode_bytes(bytes, source_name, decode_threads_,
                        options.copy_metadata_payloads.value_or(false), options.native_pixels, options.gain_map_mode,
                        options.diagnostics, options.stop_token);
  }

  awj::compat::expected<ImageDecodeResult, std::string> decode(
      const fs::path& path) const override {
    return decode(path, {});
  }

  awj::compat::expected<ImageDecodeResult, std::string> decode(
      const fs::path& path, DecodeOptions options) const override {
    try {
      auto file_io = avif_aom_detail::make_avif_file_io(path);
      if (!file_io) {
        return awj::compat::unexpected{file_io.error()};
      }
      return decode_file(**file_io, display_path_for_user(path), decode_threads_,
          options.copy_metadata_payloads.value_or(true), options.native_pixels, options.gain_map_mode,
          options.diagnostics, options.stop_token);
    } catch (const std::bad_alloc&) {
      return awj::compat::unexpected{"AVIF 解码内存不足。"};
    } catch (const std::length_error&) {
      return awj::compat::unexpected{"AVIF 解码数据超过运行时限制。"};
    } catch (const std::filesystem::filesystem_error&) {
      return awj::compat::unexpected{"AVIF 解码文件系统访问失败。"};
    }
  }

 private:
  static awj::compat::expected<void, std::string> reject_unsupported_sequence(
      const avifDecoder& decoder,
      std::string_view source_name) {
    if (decoder.imageSequenceTrackPresent && decoder.requestedSource != AVIF_DECODER_SOURCE_PRIMARY_ITEM) {
      return awj::compat::unexpected{awj::compat::format("AVIF sequence 不能无损直通，需解码首帧: {}", source_name)};
    }
    if (decoder.imageSequenceTrackPresent) {
      return awj::compat::unexpected{"AVIF 混合容器必须重新编码独立静态主图，不能将附带视频原样直通。"};
    }
    return {};
  }

  static awj::compat::expected<ImageBuffer, std::string> parse_container_decoder(
      avifDecoder& decoder,
      std::string_view source_name,
      bool copy_metadata_payloads) {
    avif_aom_detail::configure_decoder(decoder, copy_metadata_payloads);
    if (auto parsed = avif_aom_detail::parse_primary_preferred(decoder); !parsed)
      return awj::compat::unexpected{parsed.error()};
    if (auto supported = reject_unsupported_sequence(decoder, source_name); !supported) {
      return awj::compat::unexpected{supported.error()};
    }
    if (decoder.image && decoder.image->gainMap) {
      return awj::compat::unexpected{"Gain Map: ISO 21496-1 AVIF: 含增益图的输入必须合成，不能原样直通。"};
    }
    const avifImage* image = decoder.image;
    if (image == nullptr) {
      return awj::compat::unexpected{awj::compat::format("AVIF 图像信息为空: {}", source_name)};
    }
    auto dimensions = decoder_common::make_image_dimensions_checked(image->width,
                                                                    image->height,
                                                                    "AVIF");
    if (!dimensions) {
      return awj::compat::unexpected{dimensions.error()};
    }

    ImageBuffer out{.width = dimensions->width,
                    .height = dimensions->height,
                    .pixel_format = avif_aom_detail::pixel_format_from_avif(image->yuvFormat),
                    .alpha_mode = decoder.alphaPresent
                                      ? (image->alphaPremultiplied == AVIF_TRUE
                                             ? AlphaMode::premultiplied
                                             : AlphaMode::straight)
                                      : AlphaMode::none,
                    .bit_depth = static_cast<int>(image->depth),
                    .source_info = ImageSourceInfo{
                        .pixel_format = avif_aom_detail::pixel_format_from_avif(image->yuvFormat),
                        .bit_depth = static_cast<int>(image->depth),
                        .color_primaries = avif_aom_detail::int_from_avif_color(image->colorPrimaries),
                        .transfer_characteristics = avif_aom_detail::int_from_avif_transfer(
                            image->transferCharacteristics),
                        .matrix_coefficients = avif_aom_detail::int_from_avif_matrix(
                            image->matrixCoefficients),
                        .color_range = avif_aom_detail::int_from_avif_range(image->yuvRange),
                        .content_light = avif_aom_detail::content_light_from_avif(*image),
                        .has_hdr_metadata = avif_aom_detail::has_hdr_metadata(*image),
                        .color_metadata_source = avif_aom_detail::color_metadata_source_from_avif(
                            *image)}};
    if (auto copied = avif_aom_detail::copy_avif_metadata(out, *image, copy_metadata_payloads); !copied) {
      return awj::compat::unexpected{copied.error()};
    }
    return out;
  }

  static awj::compat::expected<ImageBuffer, std::string> parse_container_file(
      avif_aom_detail::AvifFileIO& file_io,
      std::string_view source_name,
      bool copy_metadata_payloads) {
    avif_aom_detail::AvifDecoder decoder{avifDecoderCreate()};
    if (!decoder) {
      return awj::compat::unexpected{"无法创建 libavif decoder。"};
    }
    decoder->codecChoice = AVIF_CODEC_CHOICE_AUTO;
    decoder->maxThreads = 1;
    avifDecoderSetIO(decoder.get(), &file_io.io);
    return parse_container_decoder(*decoder, source_name, copy_metadata_payloads);
  }

 public:
  static awj::compat::expected<ImageDecodeResult, std::string> finish_decoded_image(
      avifImage& image,
      std::string_view source_name,
      int decode_threads,
      bool copy_metadata_payloads,
      std::string decoder_id,
      bool used_fallback, bool native_pixels = false,
      GainMapDecodeMode gain_map_mode = GainMapDecodeMode::hdr_only,
      DecodeDiagnostics* diagnostics = nullptr, awj::compat::stop_token stop = {}) {
    if (stop.stop_requested()) return awj::compat::unexpected{"任务已取消。"};
    if (native_pixels && !image.gainMap) {
      auto native = copy_yuv_pixels(image);
      if (!native) return awj::compat::unexpected{native.error()};
      native->source_info->content_light = avif_aom_detail::content_light_from_avif(image);
      native->source_info->has_hdr_metadata = avif_aom_detail::has_hdr_metadata(image);
      native->source_info->color_metadata_source = avif_aom_detail::color_metadata_source_from_avif(image);
      if (auto copied = avif_aom_detail::copy_avif_metadata(*native, image, copy_metadata_payloads); !copied)
        return awj::compat::unexpected{copied.error()};
      if (auto geometry = avif_aom_detail::apply_display_geometry(*native, image); !geometry)
        return awj::compat::unexpected{geometry.error()};
      if (image.transformFlags & (AVIF_TRANSFORM_IROT | AVIF_TRANSFORM_IMIR))
        gain_map_detail::normalize_exif_orientation(*native);
      return ImageDecodeResult{.image = std::move(*native), .decoder_id = std::move(decoder_id), .used_fallback = used_fallback};
    }
    const std::string_view decode_context = image.gainMap ? "Gain Map: ISO 21496-1 AVIF" : "AVIF decoder";
    const auto dimensions = decoder_common::make_image_dimensions_checked(image.width,
                                                                         image.height,
                                                                         decode_context);
    if (!dimensions) {
      return awj::compat::unexpected{dimensions.error()};
    }
    const int output_bit_depth = image.depth > 8 ? 16 : 8;
    const auto row_bytes = avif_aom_detail::checked_rgba_stride(
        dimensions->width, decode_context, output_bit_depth > 8 ? 2 : 1);
    if (!row_bytes) {
      return awj::compat::unexpected{row_bytes.error()};
    }
    const auto byte_count = avif_aom_detail::checked_image_bytes(
        *row_bytes, dimensions->height, decode_context);
    if (!byte_count) {
      return awj::compat::unexpected{byte_count.error()};
    }

    if (*row_bytes > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
      return awj::compat::unexpected{awj::compat::format("{} 输出 stride 超过 API 限制。", decode_context)};
    }
    const auto row_bytes_u32 = static_cast<std::uint32_t>(*row_bytes);

    ImagePlane plane{.stride = row_bytes_u32};
    auto resized = decoder_common::resize_buffer(plane.bytes, *byte_count, decode_context);
    if (!resized) {
      return awj::compat::unexpected{resized.error()};
    }

    avifRGBImage rgb{};
    avifRGBImageSetDefaults(&rgb, &image);
    rgb.format = AVIF_RGB_FORMAT_RGBA;
    rgb.depth = output_bit_depth;
    rgb.maxThreads = decode_threads;
    rgb.pixels = reinterpret_cast<std::uint8_t*>(plane.bytes.data());
    rgb.rowBytes = row_bytes_u32;
    rgb.alphaPremultiplied = AVIF_FALSE;
    const auto result = avifImageYUVToRGB(&image, &rgb);
    if (result != AVIF_RESULT_OK) {
      return awj::compat::unexpected{awj::compat::format("{} YUV 转 RGB 失败: {}: {}", decode_context, source_name,
                                         avifResultToString(result))};
    }

    ImageBuffer out{.width = rgb.width,
                    .height = rgb.height,
                    .pixel_format = PixelFormat::rgba,
                    .alpha_mode = image.alphaPlane != nullptr ? AlphaMode::straight : AlphaMode::none,
                    .bit_depth = output_bit_depth,
                    .source_info = ImageSourceInfo{
                        .pixel_format = avif_aom_detail::pixel_format_from_avif(image.yuvFormat),
                        .bit_depth = static_cast<int>(image.depth),
                        .color_primaries = avif_aom_detail::int_from_avif_color(image.colorPrimaries),
                        .transfer_characteristics = avif_aom_detail::int_from_avif_transfer(
                            image.transferCharacteristics),
                        .matrix_coefficients = avif_aom_detail::int_from_avif_matrix(
                            image.matrixCoefficients),
                        .color_range = avif_aom_detail::int_from_avif_range(image.yuvRange),
                        .content_light = avif_aom_detail::content_light_from_avif(image),
                        .has_hdr_metadata = avif_aom_detail::has_hdr_metadata(image),
                        .color_metadata_source = avif_aom_detail::color_metadata_source_from_avif(
                            image)}};
    if (auto copied = avif_aom_detail::copy_avif_metadata(out, image, copy_metadata_payloads || image.gainMap != nullptr); !copied) {
      return awj::compat::unexpected{image.gainMap ? "Gain Map: ISO 21496-1 AVIF: " + copied.error() : copied.error()};
    }
    out.planes.push_back(std::move(plane));
    if (diagnostics) {
      if (!diagnostics->source_info) capture_decode_source(*diagnostics, out);
      diagnostics->decoder_id = decoder_id;
      diagnostics->stage = image.gainMap ? "avif.gain-map.render" : "complete";
      diagnostics->source_info->source_has_gain_map = image.gainMap != nullptr;
      if (image.gainMap) diagnostics->source_info->has_hdr_metadata = true;
    }
    ImageDecodeResult decoded{.image = std::move(out), .decoder_id = std::move(decoder_id),
        .used_fallback = used_fallback};
    if (image.gainMap) {
      auto* map = image.gainMap;
      if (!map->image || map->image->gainMap || !map->baseHdrHeadroom.d || !map->alternateHdrHeadroom.d ||
          image.icc.size || map->altICC.size)
        return awj::compat::unexpected{"Gain Map: ISO 21496-1 AVIF: 增益图缺少像素/有效 headroom，或使用暂不支持的 ICC 合成空间。"};
      // 辅助图只是 raw gain samples，不能当普通照片做 HDR 或色调转换。
      auto gain = finish_decoded_image(*map->image, source_name, decode_threads, false, "libavif-gain-map-samples", false);
      if (!gain) return awj::compat::unexpected{"Gain Map: ISO 21496-1 AVIF: " + gain.error()};
      const int budget = avif_aom_detail::codec_thread_count(decode_threads);
      const ResourcePlan resources{.encoder_threads_per_file = budget, .global_thread_budget = budget};
      auto pair = render_iso_gain_map(std::move(decoded.image), gain->image, *map, gain_map_mode,
          stop, resources, diagnostics ? &diagnostics->timing : nullptr);
      if (!pair) return awj::compat::unexpected{pair.error()};
      pair->decoder_id = decoded.decoder_id + "-iso21496-1-gain-map-enhanced";
      pair->used_fallback = used_fallback;
      const auto transform = [&](ImageBuffer& rendition) -> awj::compat::expected<void, std::string> {
        if (image.transformFlags & AVIF_TRANSFORM_CLAP) {
          avifCropRect crop{};
          avifDiagnostics diag{};
          if (!avifCropRectConvertCleanApertureBox(&crop, &image.clap, image.width, image.height, image.yuvFormat, &diag))
            return awj::compat::unexpected{"Gain Map: ISO 21496-1 AVIF: clean aperture 无效。"};
          if (auto cropped = gain_map_detail::crop_rgba(rendition, crop.x, crop.y, crop.width, crop.height); !cropped)
            return awj::compat::unexpected{cropped.error()};
        }
        if (auto oriented = gain_map_detail::transform_rgba(rendition,
                image.transformFlags & AVIF_TRANSFORM_IROT ? image.irot.angle : 0,
                image.transformFlags & AVIF_TRANSFORM_IMIR ? image.imir.axis : -1); !oriented)
          return awj::compat::unexpected{oriented.error()};
        if (image.transformFlags & (AVIF_TRANSFORM_IROT | AVIF_TRANSFORM_IMIR))
          gain_map_detail::normalize_exif_orientation(rendition);
        else if (auto exif = gain_map_detail::apply_exif_orientation(rendition); !exif)
          return awj::compat::unexpected{exif.error()};
        return {};
      };
      if (auto transformed = transform(pair->image); !transformed) return awj::compat::unexpected{transformed.error()};
      if (pair->gain_map_alternative) {
        if (auto transformed = transform(pair->gain_map_alternative->image); !transformed)
          return awj::compat::unexpected{transformed.error()};
      }
      decoded = std::move(*pair);
    }
    if (stop.stop_requested()) return awj::compat::unexpected{"任务已取消。"};
    if (diagnostics) {
      diagnostics->decoder_id = decoded.decoder_id;
      diagnostics->stage = "complete";
    }
    if (!copy_metadata_payloads) {
      decoded.image.metadata.clear();
      if (decoded.gain_map_alternative) decoded.gain_map_alternative->image.metadata.clear();
    }
    return decoded;
  }

 private:
  static constexpr std::array<avifCodecChoice, 2> decode_codec_choices() noexcept {
    return {AVIF_CODEC_CHOICE_DAV1D, AVIF_CODEC_CHOICE_AOM};
  }

  static awj::compat::expected<ImageDecodeResult, std::string> decode_file(
      avif_aom_detail::AvifFileIO& file_io,
      std::string_view source_name,
      int decode_threads, bool copy_metadata_payloads = true, bool native_pixels = false,
      GainMapDecodeMode gain_map_mode = GainMapDecodeMode::hdr_only,
      DecodeDiagnostics* diagnostics = nullptr, awj::compat::stop_token stop = {}) {
    try {
      const auto clamped_decode_threads = avif_aom_detail::codec_thread_count(decode_threads);
      std::string last_error, iso_error;
      for (const auto codec_choice : decode_codec_choices()) {
        if (stop.stop_requested()) return awj::compat::unexpected{"任务已取消。"};
        const char* codec_name = avifCodecName(codec_choice, AVIF_CODEC_FLAG_CAN_DECODE);
        if (codec_name == nullptr) {
          continue;
        }
        avif_aom_detail::AvifDecoder decoder{avifDecoderCreate()};
        if (!decoder) {
          return awj::compat::unexpected{"无法创建 libavif decoder。"};
        }
        decoder->codecChoice = codec_choice;
        decoder->maxThreads = clamped_decode_threads;
        avif_aom_detail::configure_decoder(*decoder, copy_metadata_payloads);
        avifDecoderSetIO(decoder.get(), &file_io.io);
        if (auto parsed = avif_aom_detail::parse_primary_preferred(*decoder); !parsed)
          return awj::compat::unexpected{parsed.error()};
        auto result = avifDecoderNextImage(decoder.get());
        if (result != AVIF_RESULT_OK) {
          last_error = awj::compat::format("AVIF {} 解码失败: {}: {}", codec_name, source_name,
                                   avif_aom_detail::avif_decode_error(result, decoder.get()));
          if (gain_map_detail::avif_iso_gain_map_failure(result, decoder.get())) {
            last_error = awj::compat::format("Gain Map: ISO 21496-1 AVIF: {}", last_error);
            iso_error = last_error;
          }
          continue;
        }
        if (decoder->image == nullptr) {
          last_error = awj::compat::format("AVIF {} 解码后图像信息为空: {}", codec_name, source_name);
          continue;
        }
        return finish_decoded_image(
            *decoder->image, source_name, clamped_decode_threads, copy_metadata_payloads,
            awj::compat::format("libavif-{}", codec_name),
            codec_choice != AVIF_CODEC_CHOICE_DAV1D, native_pixels, gain_map_mode, diagnostics, stop);
      }
      if (!iso_error.empty()) return awj::compat::unexpected{std::move(iso_error)};
      return awj::compat::unexpected{last_error.empty()
                                 ? "当前 libavif 构建没有可用的 dav1d 或 AOM decoder。"
                                 : std::move(last_error)};
    } catch (const std::bad_alloc&) {
      return awj::compat::unexpected{"AVIF 解码内存不足。"};
    } catch (const std::length_error&) {
      return awj::compat::unexpected{"AVIF 解码数据超过运行时限制。"};
    } catch (const std::filesystem::filesystem_error&) {
      return awj::compat::unexpected{"AVIF 解码文件系统访问失败。"};
    }
  }

  static awj::compat::expected<ImageDecodeResult, std::string> decode_bytes(
      std::span<const std::byte> bytes,
      std::string_view source_name,
      int decode_threads,
      bool copy_metadata_payloads = true, bool native_pixels = false,
      GainMapDecodeMode gain_map_mode = GainMapDecodeMode::hdr_only,
      DecodeDiagnostics* diagnostics = nullptr, awj::compat::stop_token stop = {}) {
    try {
      if (bytes.empty()) {
        return awj::compat::unexpected{awj::compat::format("AVIF 输入为空: {}", source_name)};
      }
      const auto clamped_decode_threads = avif_aom_detail::codec_thread_count(decode_threads);
      std::string last_error, iso_error;
      for (const auto codec_choice : decode_codec_choices()) {
        if (stop.stop_requested()) return awj::compat::unexpected{"任务已取消。"};
        const char* codec_name = avifCodecName(codec_choice, AVIF_CODEC_FLAG_CAN_DECODE);
        if (codec_name == nullptr) {
          continue;
        }
        avif_aom_detail::AvifDecoder decoder{avifDecoderCreate()};
        if (!decoder) {
          return awj::compat::unexpected{"无法创建 libavif decoder。"};
        }
        decoder->codecChoice = codec_choice;
        decoder->maxThreads = clamped_decode_threads;
        avif_aom_detail::configure_decoder(*decoder, copy_metadata_payloads);

        auto result = avifDecoderSetIOMemory(
            decoder.get(), reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
        if (result == AVIF_RESULT_OK) {
          if (auto parsed = avif_aom_detail::parse_primary_preferred(*decoder); !parsed)
            return awj::compat::unexpected{parsed.error()};
        }
        if (result == AVIF_RESULT_OK) {
          result = avifDecoderNextImage(decoder.get());
        }
        if (result != AVIF_RESULT_OK) {
          last_error = awj::compat::format("AVIF {} 解码失败: {}: {}", codec_name, source_name,
                                   avif_aom_detail::avif_decode_error(result, decoder.get()));
          if (gain_map_detail::avif_iso_gain_map_failure(result, decoder.get())) {
            last_error = awj::compat::format("Gain Map: ISO 21496-1 AVIF: {}", last_error);
            iso_error = last_error;
          }
          continue;
        }
        if (decoder->image == nullptr) {
          last_error = awj::compat::format("AVIF {} 解码后图像信息为空: {}", codec_name, source_name);
          continue;
        }
        return finish_decoded_image(
            *decoder->image, source_name, clamped_decode_threads, copy_metadata_payloads,
            awj::compat::format("libavif-{}", codec_name),
            codec_choice != AVIF_CODEC_CHOICE_DAV1D, native_pixels, gain_map_mode, diagnostics, stop);
      }
      if (!iso_error.empty()) return awj::compat::unexpected{std::move(iso_error)};
      return awj::compat::unexpected{last_error.empty()
                                 ? "当前 libavif 构建没有可用的 dav1d 或 AOM decoder。"
                                 : std::move(last_error)};
    } catch (const std::bad_alloc&) {
      return awj::compat::unexpected{"AVIF 解码内存不足。"};
    } catch (const std::length_error&) {
      return awj::compat::unexpected{"AVIF 解码数据超过运行时限制。"};
    } catch (const std::filesystem::filesystem_error&) {
      return awj::compat::unexpected{"AVIF 解码文件系统访问失败。"};
    }
  }
  int decode_threads_{1};
};

std::unique_ptr<ImageDecoder> make_avif_image_decoder(int decode_threads) {
  return std::make_unique<AvifImageDecoder>(decode_threads);
}

awj::compat::expected<bool, std::string> avif_has_sequence(const std::filesystem::path& path) {
  auto io = avif_aom_detail::make_avif_file_io(path);
  if (!io) return awj::compat::unexpected{io.error()};
  avif_aom_detail::AvifDecoder decoder{avifDecoderCreate()};
  if (!decoder) return awj::compat::unexpected{"无法创建 AVIF 动画探测器。"};
  avif_aom_detail::configure_decoder(*decoder, false);
  avifDecoderSetIO(decoder.get(), &(*io)->io);
  if (auto parsed = avif_aom_detail::parse_primary_preferred(*decoder); !parsed)
    return awj::compat::unexpected{parsed.error()};
  return decoder->imageSequenceTrackPresent == AVIF_TRUE && decoder->requestedSource == AVIF_DECODER_SOURCE_TRACKS;
}

class AvifAnimationReader final : public AnimationReader {
 public:
  std::unique_ptr<avif_aom_detail::AvifFileIO> io;
  avif_aom_detail::AvifDecoder decoder{avifDecoderCreate()};
  AnimationInfo description;
  ImageBuffer current;
  std::uint64_t budget{};
  std::size_t index{};
  const AnimationInfo& info() const noexcept override { return description; }
  const ImageBuffer& frame() const noexcept override { return current; }
  std::uint64_t memory_usage_bytes() const noexcept override {
    // Codec reference surfaces plus conversion workspace, conservatively reserved.
    return static_cast<std::uint64_t>(description.width) * description.height * 48;
  }
  awj::compat::expected<bool, std::string> next(awj::compat::stop_token stop) override {
    if (stop.stop_requested()) return awj::compat::unexpected{"任务已取消。"};
    if (index == description.durations.size()) return false;
    current = {};
    const auto result = avifDecoderNextImage(decoder.get());
    if (result != AVIF_RESULT_OK) return awj::compat::unexpected{avif_aom_detail::avif_decode_error(result, decoder.get())};
    if (decoder->image->gainMap) return awj::compat::unexpected{"Gain Map: ISO 21496-1 AVIF: 暂不支持序列逐帧增益图。"};
    auto frame = AvifImageDecoder::finish_decoded_image(*decoder->image, "AVIF animation", 1, true, "libavif-sequence", false);
    if (!frame) return awj::compat::unexpected{frame.error()};
    current = std::move(frame->image);
    const auto& image = *decoder->image;
    if (image.transformFlags & AVIF_TRANSFORM_CLAP) {
      avifCropRect crop{}; avifDiagnostics diagnostics{};
      if (!avifCropRectConvertCleanApertureBox(&crop, &image.clap, image.width, image.height, image.yuvFormat, &diagnostics))
        return awj::compat::unexpected{"动画 AVIF clean aperture 无效。"};
      auto cropped = gain_map_detail::crop_rgba(current, crop.x, crop.y, crop.width, crop.height);
      if (!cropped) return awj::compat::unexpected{cropped.error()};
    }
    auto oriented = gain_map_detail::transform_rgba(current,
        image.transformFlags & AVIF_TRANSFORM_IROT ? image.irot.angle : 0,
        image.transformFlags & AVIF_TRANSFORM_IMIR ? image.imir.axis : -1);
    if (!oriented) return awj::compat::unexpected{oriented.error()};
    if (image.transformFlags & (AVIF_TRANSFORM_IROT | AVIF_TRANSFORM_IMIR)) gain_map_detail::normalize_exif_orientation(current);
    else if (auto exif = gain_map_detail::apply_exif_orientation(current); !exif) return awj::compat::unexpected{exif.error()};
    if (index == 0) { description.width = current.width; description.height = current.height; }
    if (stop.stop_requested()) return awj::compat::unexpected{"任务已取消。"};
    ++index;
    return true;
  }
};

awj::compat::expected<std::unique_ptr<AnimationReader>, std::string> open_avif_animation(
    const std::filesystem::path& path, std::uint64_t budget, awj::compat::stop_token stop) {
  if (stop.stop_requested()) return awj::compat::unexpected{"任务已取消。"};
  auto reader = std::make_unique<AvifAnimationReader>();
  auto io = avif_aom_detail::make_avif_file_io(path);
  if (!io) return awj::compat::unexpected{io.error()};
  reader->io = std::move(*io); reader->budget = budget;
  if (!reader->decoder) return awj::compat::unexpected{"无法创建 AVIF 动画解码器。"};
  avif_aom_detail::configure_decoder(*reader->decoder, true);
  reader->decoder->maxThreads = 1;
  avifDecoderSetIO(reader->decoder.get(), &reader->io->io);
  if (auto parsed = avif_aom_detail::parse_primary_preferred(*reader->decoder); !parsed)
    return awj::compat::unexpected{parsed.error()};
  if (!reader->decoder->imageSequenceTrackPresent || reader->decoder->requestedSource != AVIF_DECODER_SOURCE_TRACKS)
    return std::unique_ptr<AnimationReader>{};
  auto& info = reader->description;
  info.width = reader->decoder->image->width; info.height = reader->decoder->image->height;
  if (!info.width || !info.height || (budget && info.width > budget / 80 / info.height))
    return awj::compat::unexpected{"动画 AVIF 解码工作区超出内存预算。"};
  if (reader->decoder->imageCount <= 0 || reader->decoder->imageCount > 100000)
    return awj::compat::unexpected{"动画 AVIF 帧数超过限制。"};
  info.decoder_id = "libavif-sequence";
  info.repetitions = reader->decoder->repetitionCount;
  if (info.repetitions == AVIF_REPETITION_COUNT_UNKNOWN) info.repetitions = 0;
  for (int i = 0; i < reader->decoder->imageCount; ++i) {
    avifImageTiming timing{};
    if (avifDecoderNthImageTiming(reader->decoder.get(), i, &timing) != AVIF_RESULT_OK || !timing.timescale || !timing.durationInTimescales)
      return awj::compat::unexpected{"动画 AVIF 帧时长无效。"};
    const auto divisor = std::gcd(timing.timescale, timing.durationInTimescales);
    const auto numerator = timing.durationInTimescales / divisor, denominator = timing.timescale / divisor;
    if (numerator > UINT32_MAX || denominator > UINT32_MAX) return awj::compat::unexpected{"动画 AVIF 帧时长精度超出支持范围。"};
    info.durations.push_back({static_cast<std::uint32_t>(numerator), static_cast<std::uint32_t>(denominator)});
  }
  return std::unique_ptr<AnimationReader>{std::move(reader)};
}

awj::compat::expected<ImageBuffer, std::string> parse_avif_container_info(const std::filesystem::path& path) {
  return AvifImageDecoder{}.parse_container_info(path);
}

std::unique_ptr<ImageEncoder> make_avif_image_encoder(AvifEncoderMode mode) {
  switch (mode) {
    case AvifEncoderMode::aom:
    case AvifEncoderMode::automatic:
      return std::make_unique<AvifLibavifImageEncoder>(mode);
    default:
      return nullptr;
  }
}
}  // namespace awj
