module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <setjmp.h>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <lib/jpegli/decode.h>
#include <lib/jpegli/encode.h>

export module awj.jpegli_codec;

import awj.codec;
import awj.config;
import awj.core;
import awj.decoder_common;
import awj.encoding_defaults;
import awj.image;
import awj.jpeg_codec;
import awj.large_image_plan;

export namespace awj {

namespace jpegli_detail {

struct DecompressCleanup {
  jpeg_decompress_struct* cinfo{};
  bool created{};

  ~DecompressCleanup() {
    if (created && cinfo != nullptr) {
      jpegli_destroy_decompress(cinfo);
    }
  }
};

struct CompressCleanup {
  jpeg_compress_struct* cinfo{};
  bool created{};

  ~CompressCleanup() {
    if (created && cinfo != nullptr) {
      jpegli_destroy_compress(cinfo);
    }
  }
};

struct MallocCleanup {
  unsigned char* data{};

  ~MallocCleanup() {
    std::free(data);
  }
};

struct DecodeContext {
  std::vector<std::byte> rgba{};
  std::vector<std::byte> icc_profile{};
  std::vector<MetadataBlock> app1_metadata{};
};

std::expected<const ImagePlane*, std::string> rgba_plane(const ImageBuffer& image) {
  if (image.pixel_format != PixelFormat::rgba || image.bit_depth != 8 ||
      image.planes.empty()) {
    return std::unexpected{"JPGLI encoder 当前需要 8-bit RGBA ImageBuffer。"};
  }
  const auto& plane = image.planes.front();
  const auto expected_stride =
      decoder_common::checked_rgba_stride(image.width, "JPGLI encoder");
  if (!expected_stride) {
    return std::unexpected{expected_stride.error()};
  }
  if (plane.stride < *expected_stride) {
    return std::unexpected{"JPGLI encoder 输入 RGBA stride 无效。"};
  }
  const auto expected_bytes =
      decoder_common::checked_image_bytes(plane.stride, image.height, "JPGLI encoder");
  if (!expected_bytes) {
    return std::unexpected{expected_bytes.error()};
  }
  if (plane.bytes.size() < *expected_bytes) {
    return std::unexpected{"JPGLI encoder 输入 RGBA buffer 尺寸无效。"};
  }
  return &plane;
}

std::expected<std::vector<std::byte>, std::string> make_rgb_buffer(
    const ImagePlane& plane, std::size_t width, std::size_t height) {
  if (width == 0 || width > std::numeric_limits<std::size_t>::max() / 3) {
    return std::unexpected{"JPGLI encoder RGB 输入宽度无效。"};
  }
  const auto rgb_stride = width * 3;
  const auto rgb_size =
      decoder_common::checked_image_bytes(rgb_stride, height, "JPGLI encoder RGB");
  if (!rgb_size) {
    return std::unexpected{rgb_size.error()};
  }
  auto rgb = decoder_common::make_byte_buffer(*rgb_size, "JPGLI encoder RGB");
  if (!rgb) {
    return std::unexpected{rgb.error()};
  }
  const auto* rgba_data = reinterpret_cast<const std::uint8_t*>(plane.bytes.data());
  auto* rgb_data = reinterpret_cast<std::uint8_t*>(rgb->data());
  for (std::size_t y = 0; y < height; ++y) {
    const auto* rgba_row = rgba_data + y * plane.stride;
    auto* rgb_row = rgb_data + y * rgb_stride;
    for (std::size_t x = 0; x < width; ++x) {
      rgb_row[x * 3] = rgba_row[x * 4];
      rgb_row[x * 3 + 1] = rgba_row[x * 4 + 1];
      rgb_row[x * 3 + 2] = rgba_row[x * 4 + 2];
    }
  }
  return rgb;
}

const MetadataBlock* first_metadata(const ImageBuffer& image, MetadataKind kind) noexcept {
  for (const auto& block : image.metadata) {
    if (block.kind == kind && !block.bytes.empty()) {
      return &block;
    }
  }
  return nullptr;
}

void apply_chroma_sampling(jpeg_compress_struct& cinfo, ChromaMode chroma) {
  if (chroma == ChromaMode::auto_keep || cinfo.num_components < 3 ||
      cinfo.comp_info == nullptr) {
    return;
  }
  int h = 1;
  int v = 1;
  switch (chroma) {
    case ChromaMode::yuv420:
      h = 2;
      v = 2;
      break;
    case ChromaMode::yuv422:
      h = 2;
      v = 1;
      break;
    case ChromaMode::yuv444:
    case ChromaMode::auto_keep:
    default:
      h = 1;
      v = 1;
      break;
  }
  cinfo.comp_info[0].h_samp_factor = h;
  cinfo.comp_info[0].v_samp_factor = v;
  for (int i = 1; i < cinfo.num_components; ++i) {
    cinfo.comp_info[i].h_samp_factor = 1;
    cinfo.comp_info[i].v_samp_factor = 1;
  }
}

std::expected<void, std::string> write_app1_marker(jpeg_compress_struct& cinfo,
                                                   const char* signature,
                                                   std::size_t signature_size,
                                                   std::span<const std::byte> payload,
                                                   std::string_view label) {
  if (payload.size() > encoding_defaults::jpeg_marker_payload_max_bytes ||
      signature_size >
          encoding_defaults::jpeg_marker_payload_max_bytes - payload.size()) {
    return std::unexpected{std::format("JPGLI {} metadata 超过单个 JPEG marker 上限。", label)};
  }
  std::vector<JOCTET> marker;
  try {
    marker.resize(signature_size + payload.size());
  } catch (const std::bad_alloc&) {
    return std::unexpected{std::format("JPGLI {} metadata buffer 内存不足。", label)};
  } catch (const std::length_error&) {
    return std::unexpected{std::format("JPGLI {} metadata buffer 尺寸超过运行时限制。", label)};
  }
  std::ranges::copy_n(reinterpret_cast<const JOCTET*>(signature), signature_size,
                      marker.begin());
  std::ranges::copy_n(reinterpret_cast<const JOCTET*>(payload.data()), payload.size(),
                      marker.begin() + static_cast<std::ptrdiff_t>(signature_size));
  jpegli_write_marker(&cinfo, JPEG_APP0 + 1, marker.data(),
                      static_cast<unsigned int>(marker.size()));
  return {};
}

std::expected<void, std::string> write_metadata(jpeg_compress_struct& cinfo,
                                                const ImageBuffer& image,
                                                const NativeEncodeSettings& settings) {
  if (settings.strip_metadata) {
    return {};
  }
  if (!settings.jpegli_xyb && settings.applied_icc == "kept") {
    if (const auto* icc = first_metadata(image, MetadataKind::icc); icc != nullptr) {
      if (icc->bytes.size() > encoding_defaults::codec_metadata_max_bytes ||
          icc->bytes.size() > std::numeric_limits<unsigned int>::max()) {
        return std::unexpected{"JPGLI ICC profile 超过运行时上限。"};
      }
      jpegli_write_icc_profile(&cinfo,
                               reinterpret_cast<const JOCTET*>(icc->bytes.data()),
                               static_cast<unsigned int>(icc->bytes.size()));
    }
  }
  if (const auto* exif = first_metadata(image, MetadataKind::exif); exif != nullptr) {
    static constexpr char exif_signature[] = "Exif\0\0";
    if (auto written = write_app1_marker(cinfo, exif_signature,
                                         sizeof(exif_signature) - 1, exif->bytes,
                                         "Exif");
        !written) {
      return std::unexpected{written.error()};
    }
  }
  if (const auto* xmp = first_metadata(image, MetadataKind::xmp); xmp != nullptr) {
    static constexpr char xmp_signature[] = "http://ns.adobe.com/xap/1.0/\0";
    if (auto written = write_app1_marker(cinfo, xmp_signature,
                                         sizeof(xmp_signature) - 1, xmp->bytes,
                                         "XMP");
        !written) {
      return std::unexpected{written.error()};
    }
  }
  return {};
}

std::expected<std::vector<std::byte>, std::string> copy_icc_profile(
    jpeg_decompress_struct& cinfo) {
  JOCTET* icc_data = nullptr;
  unsigned int icc_size = 0;
  const auto has_profile = jpegli_read_icc_profile(&cinfo, &icc_data, &icc_size);
  MallocCleanup cleanup{.data = icc_data};
  if (!has_profile || icc_data == nullptr || icc_size == 0) {
    return std::vector<std::byte>{};
  }
  auto bytes = decoder_common::make_byte_buffer(icc_size, "JPGLI ICC profile");
  if (!bytes) {
    return std::unexpected{bytes.error()};
  }
  std::ranges::copy_n(reinterpret_cast<const std::byte*>(icc_data), icc_size,
                      bytes->begin());
  return std::move(*bytes);
}

std::expected<ImageDecodeResult, std::string> decode_bytes(
    std::span<const std::byte> bytes, std::string_view source_name,
    bool copy_metadata_payloads) {
  try {
    if (bytes.empty()) {
      return std::unexpected{std::format("JPGLI 输入为空: {}", source_name)};
    }
    if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<unsigned long>::max())) {
      return std::unexpected{"JPGLI 输入超过 libjpeg API 限制。"};
    }

    std::unique_ptr<jpegli_detail::DecodeContext> context;
    try {
      context = std::make_unique<jpegli_detail::DecodeContext>();
    } catch (const std::bad_alloc&) {
      return std::unexpected{"JPGLI decoder 内存不足。"};
    }

    jpeg_decompress_struct cinfo{};
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324 4611)
#endif
    jpeg_detail::ErrorManager error{};
    cinfo.err = jpegli_std_error(&error.pub);
    error.pub.error_exit = jpeg_detail::error_exit;
    DecompressCleanup cleanup{.cinfo = &cinfo};
    if (setjmp(error.jump) != 0) {
      return std::unexpected{std::format("JPGLI 解码失败: {}", error.message)};
    }
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

    jpegli_create_decompress(&cinfo);
    cleanup.created = true;
    std::vector<std::byte> owned_bytes{bytes.begin(), bytes.end()};
    if (auto metadata_budget = jpeg_detail::validate_saved_metadata_budget_from_memory(
            owned_bytes, source_name, copy_metadata_payloads);
        !metadata_budget) {
      return std::unexpected{metadata_budget.error()};
    }
    jpegli_mem_src(&cinfo, reinterpret_cast<const unsigned char*>(bytes.data()),
                   static_cast<unsigned long>(bytes.size()));
    if (copy_metadata_payloads) {
      jpegli_save_markers(&cinfo, JPEG_APP0 + 1, 0xFFFFu);
    }
    jpegli_save_markers(&cinfo, JPEG_APP0 + 2, 0xFFFFu);
    if (jpegli_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
      return std::unexpected{std::format("JPGLI/JPEG 文件头无效: {}", source_name)};
    }
    if (auto mpf = jpeg_detail::reject_multi_picture_mpf(cinfo, source_name); !mpf) {
      return std::unexpected{mpf.error()};
    }
    const auto source_info = jpeg_detail::source_info_from_header(cinfo);
    auto icc_profile = jpegli_detail::copy_icc_profile(cinfo);
    if (!icc_profile) {
      return std::unexpected{icc_profile.error()};
    }
    context->icc_profile = std::move(*icc_profile);
    if (copy_metadata_payloads) {
      ImageBuffer metadata_image{};
      if (auto copied = jpeg_detail::copy_app1_metadata(metadata_image, cinfo); !copied) {
        return std::unexpected{copied.error()};
      }
      context->app1_metadata = std::move(metadata_image.metadata);
    }
    cinfo.out_color_space = JCS_EXT_RGBA;
    jpegli_start_decompress(&cinfo);
    if (cinfo.output_width == 0 || cinfo.output_height == 0 ||
        cinfo.output_components != 4) {
      return std::unexpected{std::format("JPGLI 输出格式无效: {}", source_name)};
    }
    const auto width = static_cast<std::size_t>(cinfo.output_width);
    const auto height = static_cast<std::size_t>(cinfo.output_height);
    const auto stride = decoder_common::checked_rgba_stride(width, "JPGLI decoder");
    if (!stride) {
      return std::unexpected{stride.error()};
    }
    const auto byte_count =
        decoder_common::checked_image_bytes(*stride, height, "JPGLI decoder");
    if (!byte_count) {
      return std::unexpected{byte_count.error()};
    }
    auto rgba = decoder_common::make_byte_buffer(*byte_count, "JPGLI decoder");
    if (!rgba) {
      return std::unexpected{rgba.error()};
    }
    context->rgba = std::move(*rgba);

    while (cinfo.output_scanline < cinfo.output_height) {
      auto* row =
          reinterpret_cast<JSAMPROW>(context->rgba.data() + cinfo.output_scanline * *stride);
      if (jpegli_read_scanlines(&cinfo, &row, 1) != 1) {
        return std::unexpected{std::format("JPGLI 扫描行读取未推进: {}", source_name)};
      }
    }
    jpegli_finish_decompress(&cinfo);

    auto image = decoder_common::make_rgba_image(width, height, std::move(context->rgba),
                                                 AlphaMode::none, "JPGLI decoder", source_info);
    if (!image) {
      return std::unexpected{image.error()};
    }
    if (!context->icc_profile.empty()) {
      image->metadata.push_back(MetadataBlock{.kind = MetadataKind::icc,
                                              .bytes = std::move(context->icc_profile)});
    }
    if (copy_metadata_payloads) {
      image->metadata.insert(image->metadata.end(),
                             std::make_move_iterator(context->app1_metadata.begin()),
                             std::make_move_iterator(context->app1_metadata.end()));
    }
    return ImageDecodeResult{.image = std::move(*image), .decoder_id = "jpegli"};
  } catch (const std::bad_alloc&) {
    return std::unexpected{"JPGLI 解码内存不足。"};
  } catch (const std::length_error&) {
    return std::unexpected{"JPGLI 解码数据超过运行时限制。"};
  } catch (const std::filesystem::filesystem_error&) {
    return std::unexpected{"JPGLI 解码文件系统访问失败。"};
  }
}

}  // namespace jpegli_detail

class JpegliImageDecoder final : public ImageDecoder {
 public:
  [[nodiscard]] std::string_view id() const noexcept override { return "jpegli"; }

  [[nodiscard]] bool can_decode(const fs::path& path) const override {
    static constexpr std::wstring_view extensions[] = {L".jpg", L".jpeg", L".jpe", L".jfif"};
    return decoder_common::extension_is_one_of(path, extensions);
  }

  std::expected<ImageDimensions, std::string> probe_dimensions(
      const fs::path& path) const override {
    try {
      jpeg_detail::FileCleanup input{jpeg_detail::open_binary_file(path)};
      if (input.file == nullptr) {
        return std::unexpected{std::format("无法读取 JPGLI/JPEG 文件: {}",
                                           display_path_for_user(path))};
      }

      jpeg_decompress_struct cinfo{};
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324 4611)
#endif
      jpeg_detail::ErrorManager error{};
      cinfo.err = jpegli_std_error(&error.pub);
      error.pub.error_exit = jpeg_detail::error_exit;
      jpegli_detail::DecompressCleanup cleanup{.cinfo = &cinfo};
      if (setjmp(error.jump) != 0) {
        return std::unexpected{std::format("JPGLI 读取尺寸失败: {}", error.message)};
      }
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
      jpegli_create_decompress(&cinfo);
      cleanup.created = true;
      if (auto metadata_budget = jpeg_detail::validate_saved_metadata_budget_from_file(
              input.file, display_path_for_user(path), false);
          !metadata_budget) {
        return std::unexpected{metadata_budget.error()};
      }
      jpegli_stdio_src(&cinfo, input.file);
      jpegli_save_markers(&cinfo, JPEG_APP0 + 2, 0xFFFFu);
      if (jpegli_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        return std::unexpected{std::format("JPGLI/JPEG 文件头无效: {}",
                                           display_path_for_user(path))};
      }
      if (auto mpf = jpeg_detail::reject_multi_picture_mpf(cinfo, display_path_for_user(path)); !mpf) {
        return std::unexpected{mpf.error()};
      }
      return decoder_common::make_image_dimensions_checked(cinfo.image_width,
                                                           cinfo.image_height,
                                                           "JPGLI");
    } catch (const std::bad_alloc&) {
      return std::unexpected{"JPGLI 尺寸探测内存不足。"};
    } catch (const std::length_error&) {
      return std::unexpected{"JPGLI 尺寸探测数据超过运行时限制。"};
    } catch (const std::filesystem::filesystem_error&) {
      return std::unexpected{"JPGLI 尺寸探测文件系统访问失败。"};
    }
  }

  std::expected<ImageDecodeResult, std::string> decode(
      const fs::path& path) const override {
    auto bytes = decoder_common::read_file_bytes(path, "JPGLI/JPEG");
    if (!bytes) {
      return std::unexpected{bytes.error()};
    }
    return jpegli_detail::decode_bytes(*bytes, display_path_for_user(path), true);
  }

  std::expected<ImageDecodeResult, std::string> decode_memory(
      std::span<const std::byte> bytes,
      std::string_view source_name,
      DecodeOptions options = {}) const override {
    return jpegli_detail::decode_bytes(
        bytes, source_name, options.copy_metadata_payloads.value_or(true));
  }
};

class JpegliImageEncoder final : public ImageEncoder {
 public:
  [[nodiscard]] std::string_view id() const noexcept override { return "jpegli"; }

  [[nodiscard]] CodecCapabilities capabilities() const override {
    return CodecCapabilities{.output_format = OutputFormat::jpgli,
                             .features = CodecFeature::visual_quality_search,
                             .min_quality = 1,
                             .max_quality = 100,
                             .min_speed = 0,
                             .max_speed = 10,
                             .bit_depths = {8}};
  }

  std::expected<NativeEncodeResult, std::string> encode(
      const ImageBuffer& image,
      const NativeEncodeSettings& settings,
      std::stop_token stop_token = {}) const override {
    try {
      if (stop_token.stop_requested()) {
        return std::unexpected{"任务已取消。"};
      }
      if (settings.jpegli_progressive_level < 0 ||
          settings.jpegli_progressive_level > 2) {
        return std::unexpected{"JPGLI progressive level 只支持 0、1、2。"};
      }
      if (settings.jpegli_progressive_level > 0 &&
          !settings.jpegli_optimize_huffman) {
        return std::unexpected{
            "JPGLI 渐进 JPEG 需要优化哈夫曼表；若要关闭优化，请将渐进设为 0。"};
      }
      auto plane = jpegli_detail::rgba_plane(image);
      if (!plane) {
        return std::unexpected{plane.error()};
      }
      if (image.width > static_cast<std::size_t>(std::numeric_limits<JDIMENSION>::max()) ||
          image.height > static_cast<std::size_t>(std::numeric_limits<JDIMENSION>::max())) {
        return std::unexpected{"JPGLI encoder 输入尺寸超过 libjpeg API 限制。"};
      }

      std::vector<std::byte> rgb;
      auto rgb_input = settings.jpegli_rgb8_input;
      if (rgb_input.empty()) {
        auto prepared_rgb =
            jpegli_detail::make_rgb_buffer(**plane, image.width, image.height);
        if (!prepared_rgb) {
          return std::unexpected{prepared_rgb.error()};
        }
        rgb = std::move(*prepared_rgb);
        rgb_input = std::span<const std::byte>{rgb};
      } else {
        const auto expected_size = decoder_common::checked_image_bytes(
            image.width * 3, image.height, "JPGLI encoder RGB");
        if (!expected_size) {
          return std::unexpected{expected_size.error()};
        }
        if (rgb_input.size() != *expected_size) {
          return std::unexpected{"JPGLI encoder RGB 输入缓存尺寸无效。"};
        }
      }

      unsigned char* output = nullptr;
      unsigned long output_size = 0;
      jpegli_detail::MallocCleanup output_cleanup{};
      jpeg_compress_struct cinfo{};
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324 4611)
#endif
      jpeg_detail::ErrorManager error{};
      cinfo.err = jpegli_std_error(&error.pub);
      error.pub.error_exit = jpeg_detail::error_exit;
      jpegli_detail::CompressCleanup cleanup{.cinfo = &cinfo};
      if (setjmp(error.jump) != 0) {
        output_cleanup.data = output;
        return std::unexpected{std::format("JPGLI 编码失败: {}", error.message)};
      }
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

      jpegli_create_compress(&cinfo);
      cleanup.created = true;
      jpegli_mem_dest(&cinfo, &output, &output_size);
      cinfo.image_width = static_cast<JDIMENSION>(image.width);
      cinfo.image_height = static_cast<JDIMENSION>(image.height);
      cinfo.input_components = 3;
      cinfo.in_color_space = JCS_RGB;
      if (settings.jpegli_xyb) {
        jpegli_set_xyb_mode(&cinfo);
      }
      jpegli_set_defaults(&cinfo);
      if (settings.jpegli_gain_map_samples) {
        jpegli_enable_adaptive_quantization(&cinfo, FALSE);
      }
      jpegli_detail::apply_chroma_sampling(cinfo, settings.chroma_mode);
      const int final_quality = std::clamp(settings.quality, 1, 100);
      jpegli_set_quality(&cinfo, final_quality, TRUE);
      jpegli_set_progressive_level(&cinfo, settings.jpegli_progressive_level);
      cinfo.optimize_coding = settings.jpegli_optimize_huffman ? TRUE : FALSE;
      jpegli_start_compress(&cinfo, TRUE);
      if (auto metadata = jpegli_detail::write_metadata(cinfo, image, settings); !metadata) {
        return std::unexpected{metadata.error()};
      }

      const auto row_stride = image.width * 3;
      const auto* data = reinterpret_cast<const JSAMPLE*>(rgb_input.data());
      while (cinfo.next_scanline < cinfo.image_height) {
        if (stop_token.stop_requested()) {
          return std::unexpected{"任务已取消。"};
        }
        JSAMPROW row = const_cast<JSAMPROW>(
            data + static_cast<std::size_t>(cinfo.next_scanline) * row_stride);
        if (jpegli_write_scanlines(&cinfo, &row, 1) != 1) {
          return std::unexpected{"JPGLI scanline 写入未推进。"};
        }
      }
      jpegli_finish_compress(&cinfo);
      if (output == nullptr || output_size == 0) {
        return std::unexpected{"JPGLI encoder 输出失败。"};
      }
      output_cleanup.data = output;

      auto bytes = decoder_common::make_byte_buffer(output_size, "JPGLI encoder output");
      if (!bytes) {
        return std::unexpected{bytes.error()};
      }
      std::ranges::copy_n(reinterpret_cast<const std::byte*>(output), output_size,
                          bytes->begin());

      auto diagnostics = diagnostics_from_settings(settings);
      diagnostics.encoder_id = "jpegli";
      diagnostics.integration_mode = "jpegli-native";
      diagnostics.encoder_threads = 1;
      diagnostics.memory_budget_bytes = settings.resources.memory_limit_bytes;
      return NativeEncodeResult{.encoded = EncodedImage{.bytes = std::move(*bytes),
                                                        .codec_name = "jpegli"},
                                .diagnostics = std::move(diagnostics),
                                .final_quality = final_quality,
                                .lossless = false,
                                .search_attempt_count = 1};
    } catch (const std::bad_alloc&) {
      return std::unexpected{"JPGLI 编码内存不足。"};
    } catch (const std::length_error&) {
      return std::unexpected{"JPGLI 编码数据超过运行时限制。"};
    } catch (const std::filesystem::filesystem_error&) {
      return std::unexpected{"JPGLI 编码文件系统访问失败。"};
    }
  }
};

}  // namespace awj
