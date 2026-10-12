module;

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <format>
#include <limits>
#include <memory>
#include <new>
#include <setjmp.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <jpeglib.h>

export module awj.jpeg_codec;

import awj.codec;
import awj.core;
import awj.decoder_common;
import awj.encoding_defaults;
import awj.image;
import awj.large_image_plan;

export namespace awj {

namespace jpeg_detail {

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
struct ErrorManager {
  jpeg_error_mgr pub{};
  jmp_buf jump{};
  char message[JMSG_LENGTH_MAX]{};
};
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

void error_exit(j_common_ptr cinfo) {
  auto* error = reinterpret_cast<ErrorManager*>(cinfo->err);
  (*cinfo->err->format_message)(cinfo, error->message);
  longjmp(error->jump, 1);
}

struct FileCleanup {
  explicit FileCleanup(std::FILE* value) noexcept : file{value} {}

  std::FILE* file{};

  ~FileCleanup() {
    if (file != nullptr) {
      std::fclose(file);
    }
  }
  FileCleanup(const FileCleanup&) = delete;
  FileCleanup& operator=(const FileCleanup&) = delete;
};

struct DecompressCleanup {
  jpeg_decompress_struct* cinfo{};
  bool created{};

  ~DecompressCleanup() {
    if (created && cinfo != nullptr) {
      jpeg_destroy_decompress(cinfo);
    }
  }
};

struct DecodeContext {
  std::vector<std::byte> rgba{};
  std::vector<std::byte> icc_profile{};
};

std::FILE* open_binary_file(const fs::path& path) noexcept {
#ifdef _WIN32
  std::FILE* file = nullptr;
  return _wfopen_s(&file, path.native().c_str(), L"rb") == 0 ? file : nullptr;
#else
  return std::fopen(path.string().c_str(), "rb");
#endif
}

constexpr int marker_tem = 0x01;
constexpr int marker_rst0 = 0xd0;
constexpr int marker_rst7 = 0xd7;
constexpr int marker_soi = 0xd8;
constexpr int marker_eoi = 0xd9;
constexpr int marker_sos = 0xda;

bool marker_payload_has_prefix(const JOCTET* data,
                               std::size_t size,
                               const char* signature,
                               std::size_t signature_size) noexcept {
  if (data == nullptr || size < signature_size) {
    return false;
  }
  for (std::size_t i = 0; i < signature_size; ++i) {
    if (data[i] != static_cast<JOCTET>(signature[i])) {
      return false;
    }
  }
  return true;
}

bool marker_has_prefix(const jpeg_marker_struct& marker,
                       const char* signature,
                       std::size_t signature_size) noexcept {
  return marker_payload_has_prefix(marker.data, marker.data_length, signature, signature_size);
}

std::expected<void, std::string> add_saved_metadata_payload(
    std::size_t payload_size,
    std::size_t& metadata_bytes,
    std::size_t& metadata_marker_count) {
  if (++metadata_marker_count >
      encoding_defaults::jpeg_max_saved_metadata_marker_count) {
    return std::unexpected{"JPEG metadata marker 数量超过上限。"};
  }
  if (payload_size > encoding_defaults::codec_metadata_max_bytes ||
      metadata_bytes >
          encoding_defaults::codec_metadata_max_bytes - payload_size) {
    return std::unexpected{"JPEG metadata 累计大小超过 64 MiB 上限。"};
  }
  metadata_bytes += payload_size;
  return {};
}

template <typename ReadByte, typename SkipPayload>
std::expected<void, std::string> validate_saved_metadata_budget_impl(
    ReadByte read_byte,
    SkipPayload skip_payload,
    std::string_view source_name,
    bool include_app1) {
  std::size_t metadata_bytes = 0;
  std::size_t metadata_marker_count = 0;
  bool found_soi = false;
  while (true) {
    const auto byte = read_byte();
    if (byte == EOF) {
      return std::unexpected{std::format("JPEG metadata 头部截断: {}", source_name)};
    }
    if (byte != 0xff) {
      if (!found_soi) {
        return std::unexpected{std::format("JPEG SOI 标记无效: {}", source_name)};
      }
      return std::unexpected{std::format("JPEG metadata 标记无效: {}", source_name)};
    }

    int marker = EOF;
    do {
      marker = read_byte();
      if (marker == EOF) {
        return std::unexpected{std::format("JPEG metadata 标记截断: {}", source_name)};
      }
    } while (marker == 0xff);

    if (marker == 0x00) {
      return std::unexpected{std::format("JPEG metadata 标记无效: {}", source_name)};
    }
    if (marker == marker_soi) {
      found_soi = true;
      continue;
    }
    if (!found_soi) {
      return std::unexpected{std::format("JPEG SOI 标记无效: {}", source_name)};
    }
    if (marker == marker_sos || marker == marker_eoi) {
      break;
    }
    if (marker == marker_tem || (marker >= marker_rst0 && marker <= marker_rst7)) {
      continue;
    }

    const auto hi = read_byte();
    const auto lo = read_byte();
    if (hi == EOF || lo == EOF) {
      return std::unexpected{std::format("JPEG metadata 长度截断: {}", source_name)};
    }
    const auto segment_length = (static_cast<unsigned int>(hi) << 8) |
                                static_cast<unsigned int>(lo);
    if (segment_length < 2) {
      return std::unexpected{std::format("JPEG metadata 长度无效: {}", source_name)};
    }
    const auto payload_size = static_cast<std::size_t>(segment_length - 2);
    const auto should_count = marker == JPEG_APP0 + 2 || (include_app1 && marker == JPEG_APP0 + 1);
    if (should_count) {
      if (auto counted = add_saved_metadata_payload(payload_size, metadata_bytes,
                                                    metadata_marker_count);
          !counted) {
        return std::unexpected{counted.error()};
      }
    }

    if (auto skipped = skip_payload(payload_size); !skipped) {
      return std::unexpected{skipped.error()};
    }
  }
  return {};
}

std::expected<void, std::string> validate_saved_metadata_budget_from_file(
    std::FILE* input,
    std::string_view source_name,
    bool include_app1) {
  if (input == nullptr) {
    return std::unexpected{std::format("无法读取 JPEG 文件: {}", source_name)};
  }
  auto result = validate_saved_metadata_budget_impl(
      [input]() { return std::fgetc(input); },
      [input, source_name](std::size_t payload_size) -> std::expected<void, std::string> {
        if (payload_size > static_cast<std::size_t>(std::numeric_limits<long>::max())) {
          return std::unexpected{
              std::format("JPEG metadata segment 超过文件 API 限制: {}", source_name)};
        }
        if (std::fseek(input, static_cast<long>(payload_size), SEEK_CUR) != 0) {
          return std::unexpected{std::format("JPEG metadata segment 截断: {}", source_name)};
        }
        return {};
      },
      source_name, include_app1);
  if (!result) {
    return std::unexpected{result.error()};
  }
  if (std::fseek(input, 0, SEEK_SET) != 0) {
    return std::unexpected{std::format("JPEG metadata 重置文件位置失败: {}", source_name)};
  }
  return {};
}

std::expected<void, std::string> validate_saved_metadata_budget_from_memory(
    const std::vector<std::byte>& bytes,
    std::string_view source_name,
    bool include_app1) {
  const auto* data = reinterpret_cast<const unsigned char*>(bytes.data());
  std::size_t offset = 0;
  return validate_saved_metadata_budget_impl(
      [&data, &offset, size = bytes.size()]() {
        if (offset >= size) {
          return EOF;
        }
        return static_cast<int>(data[offset++]);
      },
      [&offset, size = bytes.size(), source_name](std::size_t payload_size)
          -> std::expected<void, std::string> {
        if (payload_size > size - offset) {
          return std::unexpected{std::format("JPEG metadata segment 截断: {}", source_name)};
        }
        offset += payload_size;
        return {};
      },
      source_name, include_app1);
}

std::expected<std::vector<std::byte>, std::string> copy_marker_payload(
    const jpeg_marker_struct& marker,
    std::size_t payload_offset,
    std::string_view context) {
  if (marker.data == nullptr || marker.data_length <= payload_offset) {
    return std::vector<std::byte>{};
  }
  const auto payload_size = marker.data_length - payload_offset;
  auto bytes = decoder_common::make_byte_buffer(payload_size, context);
  if (!bytes) {
    return std::unexpected{bytes.error()};
  }
  std::ranges::copy_n(reinterpret_cast<const std::byte*>(marker.data + payload_offset),
                      payload_size, bytes->begin());
  return std::move(*bytes);
}

bool has_icc_signature(const jpeg_marker_struct& marker) noexcept {
  static constexpr char signature[] = "ICC_PROFILE";
  return marker.data_length >= sizeof(signature) + 2 &&
         marker_has_prefix(marker, signature, sizeof(signature));
}

std::expected<std::vector<std::byte>, std::string> copy_icc_profile(
    const jpeg_decompress_struct& cinfo) {
  std::array<const jpeg_marker_struct*, 256> markers{};
  std::size_t marker_count = 0;
  for (auto* marker = cinfo.marker_list; marker != nullptr; marker = marker->next) {
    if (marker->marker != JPEG_APP0 + 2 || !has_icc_signature(*marker)) {
      continue;
    }
    const auto sequence = static_cast<std::size_t>(marker->data[sizeof("ICC_PROFILE")]);
    const auto count = static_cast<std::size_t>(marker->data[sizeof("ICC_PROFILE") + 1]);
    if (sequence == 0 || count == 0 || sequence > count) {
      return std::vector<std::byte>{};
    }
    if (marker_count == 0) {
      marker_count = count;
    } else if (marker_count != count) {
      return std::vector<std::byte>{};
    }
    if (markers[sequence] != nullptr) {
      return std::vector<std::byte>{};
    }
    markers[sequence] = marker;
  }
  if (marker_count == 0) {
    return std::vector<std::byte>{};
  }

  std::size_t profile_size = 0;
  for (std::size_t sequence = 1; sequence <= marker_count; ++sequence) {
    const auto* marker = markers[sequence];
    if (marker == nullptr) {
      return std::vector<std::byte>{};
    }
    const auto chunk_size = marker->data_length - sizeof("ICC_PROFILE") - 2;
    if (chunk_size > std::numeric_limits<std::size_t>::max() - profile_size) {
      return std::unexpected{"JPEG ICC profile 大小超过运行时限制。"};
    }
    profile_size += chunk_size;
    if (profile_size > encoding_defaults::codec_metadata_max_bytes) {
      return std::unexpected{"JPEG ICC profile 超过 64 MiB 上限。"};
    }
  }
  if (profile_size == 0) {
    return std::vector<std::byte>{};
  }

  auto bytes = decoder_common::make_byte_buffer(profile_size, "JPEG ICC profile");
  if (!bytes) {
    return std::unexpected{bytes.error()};
  }
  std::size_t offset = 0;
  for (std::size_t sequence = 1; sequence <= marker_count; ++sequence) {
    const auto* marker = markers[sequence];
    const auto chunk_size = marker->data_length - sizeof("ICC_PROFILE") - 2;
    std::ranges::copy_n(reinterpret_cast<const std::byte*>(marker->data + sizeof("ICC_PROFILE") + 2),
                        chunk_size, bytes->begin() + static_cast<std::ptrdiff_t>(offset));
    offset += chunk_size;
  }
  return std::move(*bytes);
}

std::uint16_t read_marker_u16(const JOCTET* data, bool little_endian) noexcept {
  const auto b0 = static_cast<std::uint16_t>(data[0]);
  const auto b1 = static_cast<std::uint16_t>(data[1]);
  return little_endian ? static_cast<std::uint16_t>(b0 | (b1 << 8))
                       : static_cast<std::uint16_t>((b0 << 8) | b1);
}

std::uint32_t read_marker_u32(const JOCTET* data, bool little_endian) noexcept {
  const auto b0 = static_cast<std::uint32_t>(data[0]);
  const auto b1 = static_cast<std::uint32_t>(data[1]);
  const auto b2 = static_cast<std::uint32_t>(data[2]);
  const auto b3 = static_cast<std::uint32_t>(data[3]);
  return little_endian ? (b0 | (b1 << 8) | (b2 << 16) | (b3 << 24))
                       : ((b0 << 24) | (b1 << 16) | (b2 << 8) | b3);
}

std::expected<std::uint32_t, std::string> parse_mpf_image_count(
    const jpeg_marker_struct& marker) {
  static constexpr std::size_t mpf_signature_size = 4;
  if (marker.data == nullptr || marker.data_length < mpf_signature_size + 8) {
    return std::unexpected{"JPEG MPF metadata 头部不完整。"};
  }

  const auto* tiff = marker.data + mpf_signature_size;
  const auto tiff_size = marker.data_length - mpf_signature_size;
  bool little_endian = false;
  if (tiff[0] == static_cast<JOCTET>('I') && tiff[1] == static_cast<JOCTET>('I')) {
    little_endian = true;
  } else if (tiff[0] == static_cast<JOCTET>('M') && tiff[1] == static_cast<JOCTET>('M')) {
    little_endian = false;
  } else {
    return std::unexpected{"JPEG MPF metadata 字节序无效。"};
  }
  if (read_marker_u16(tiff + 2, little_endian) != 42) {
    return std::unexpected{"JPEG MPF metadata TIFF 标记无效。"};
  }

  const auto ifd_offset = static_cast<std::size_t>(read_marker_u32(tiff + 4, little_endian));
  if (ifd_offset > tiff_size || tiff_size - ifd_offset < 2) {
    return std::unexpected{"JPEG MPF metadata IFD 偏移无效。"};
  }
  const auto entry_count = read_marker_u16(tiff + ifd_offset, little_endian);
  if (entry_count > (tiff_size - ifd_offset - 2) / 12) {
    return std::unexpected{"JPEG MPF metadata IFD 截断。"};
  }

  static constexpr std::uint16_t number_of_images_tag = 0xb001;
  static constexpr std::uint16_t tiff_type_long = 4;
  for (std::uint16_t i = 0; i < entry_count; ++i) {
    const auto entry_offset = ifd_offset + 2 + static_cast<std::size_t>(i) * 12;
    const auto* entry = tiff + entry_offset;
    const auto tag = read_marker_u16(entry, little_endian);
    if (tag != number_of_images_tag) {
      continue;
    }
    const auto type = read_marker_u16(entry + 2, little_endian);
    const auto count = read_marker_u32(entry + 4, little_endian);
    if (type != tiff_type_long || count != 1) {
      return std::unexpected{"JPEG MPF metadata 图像数量字段无效。"};
    }
    return read_marker_u32(entry + 8, little_endian);
  }
  return std::unexpected{"JPEG MPF metadata 缺少图像数量。"};
}

std::expected<void, std::string> reject_multi_picture_mpf(
    const jpeg_decompress_struct& cinfo,
    std::string_view source_name) {
  static constexpr char mpf_signature[] = "MPF\0";
  for (auto* marker = cinfo.marker_list; marker != nullptr; marker = marker->next) {
    if (marker->marker != JPEG_APP0 + 2 ||
        !marker_has_prefix(*marker, mpf_signature, sizeof(mpf_signature) - 1)) {
      continue;
    }
    auto image_count = parse_mpf_image_count(*marker);
    if (!image_count) {
      return std::unexpected{std::format("暂不支持多图 JPEG MPF 输入: {}; {}",
                                         source_name, image_count.error())};
    }
    if (*image_count != 1) {
      return std::unexpected{std::format("暂不支持多图 JPEG MPF 输入: {}", source_name)};
    }
  }
  return {};
}

std::expected<void, std::string> copy_app1_metadata(ImageBuffer& image,
                                                    const jpeg_decompress_struct& cinfo) {
  static constexpr char exif_signature[] = "Exif\0\0";
  static constexpr char xmp_signature[] = "http://ns.adobe.com/xap/1.0/\0";

  bool copied_exif = false;
  bool copied_xmp = false;
  for (auto* marker = cinfo.marker_list; marker != nullptr; marker = marker->next) {
    if (marker->marker != JPEG_APP0 + 1) {
      continue;
    }
    if (!copied_exif && marker_has_prefix(*marker, exif_signature, sizeof(exif_signature) - 1)) {
      auto exif = copy_marker_payload(*marker, sizeof(exif_signature) - 1, "JPEG Exif metadata");
      if (!exif) {
        return std::unexpected{exif.error()};
      }
      if (!exif->empty()) {
        image.metadata.push_back(MetadataBlock{.kind = MetadataKind::exif,
                                               .bytes = std::move(*exif)});
        copied_exif = true;
      }
      continue;
    }
    if (!copied_xmp && marker_has_prefix(*marker, xmp_signature, sizeof(xmp_signature) - 1)) {
      auto xmp = copy_marker_payload(*marker, sizeof(xmp_signature) - 1, "JPEG XMP metadata");
      if (!xmp) {
        return std::unexpected{xmp.error()};
      }
      if (!xmp->empty()) {
        image.metadata.push_back(MetadataBlock{.kind = MetadataKind::xmp,
                                               .bytes = std::move(*xmp)});
        copied_xmp = true;
      }
    }
  }
  return {};
}

ImageSourceInfo source_info_from_header(const jpeg_decompress_struct& cinfo) noexcept {
  auto pixel_format = PixelFormat::unknown;
  if (cinfo.num_components == 1 || cinfo.jpeg_color_space == JCS_GRAYSCALE) {
    pixel_format = PixelFormat::gray;
  } else if (cinfo.jpeg_color_space == JCS_RGB) {
    pixel_format = PixelFormat::rgb;
  } else if (cinfo.jpeg_color_space == JCS_YCbCr && cinfo.num_components >= 3 &&
             cinfo.comp_info != nullptr) {
    const auto& y = cinfo.comp_info[0];
    const auto& cb = cinfo.comp_info[1];
    const auto& cr = cinfo.comp_info[2];
    if (y.h_samp_factor > 0 && y.v_samp_factor > 0 &&
        cb.h_samp_factor > 0 && cb.v_samp_factor > 0 &&
        cb.h_samp_factor == cr.h_samp_factor && cb.v_samp_factor == cr.v_samp_factor) {
      if (y.h_samp_factor == cb.h_samp_factor && y.v_samp_factor == cb.v_samp_factor) {
        pixel_format = PixelFormat::yuv444;
      } else if (y.h_samp_factor == cb.h_samp_factor * 2 &&
                 y.v_samp_factor == cb.v_samp_factor) {
        pixel_format = PixelFormat::yuv422;
      } else if (y.h_samp_factor == cb.h_samp_factor * 2 &&
                 y.v_samp_factor == cb.v_samp_factor * 2) {
        pixel_format = PixelFormat::yuv420;
      }
    }
  }
  return ImageSourceInfo{.pixel_format = pixel_format,
                         .bit_depth = cinfo.data_precision > 0 ? cinfo.data_precision : 0};
}

bool opaque_sdr8_yuv420_header(const jpeg_decompress_struct& cinfo) noexcept {
  const auto info = source_info_from_header(cinfo);
  if (info.bit_depth != 8 || info.pixel_format != PixelFormat::yuv420) return false;
  for (auto* marker = cinfo.marker_list; marker; marker = marker->next)
    if (marker->marker == JPEG_APP0 + 2 && has_icc_signature(*marker)) return false;
  return true;
}

}  // namespace jpeg_detail

class JpegImageDecoder final : public ImageDecoder {
 public:
  [[nodiscard]] std::string_view id() const noexcept override { return "libjpeg-turbo"; }

  [[nodiscard]] bool can_decode(const fs::path& path) const override {
    static constexpr std::wstring_view extensions[] = {L".jpg", L".jpeg", L".jpe", L".jfif"};
    return decoder_common::extension_is_one_of(path, extensions);
  }

  std::expected<ImageDimensions, std::string> probe_dimensions(
      const fs::path& path) const override {
    try {
      std::error_code ec;
      const auto file_size = fs::file_size(path, ec);
      if (ec) {
        return std::unexpected{std::format("读取 JPEG 文件大小失败: {}；系统错误：{}",
                                           display_path_for_user(path), ec.message())};
      }
      if (file_size > static_cast<std::uintmax_t>(encoding_defaults::effective_max_input_file_bytes())) {
        return std::unexpected{std::format("JPEG 文件超过当前输入上限: {}",
                                           display_path_for_user(path))};
      }

      jpeg_detail::FileCleanup input{jpeg_detail::open_binary_file(path)};
      if (input.file == nullptr) {
        return std::unexpected{std::format("无法读取 JPEG 文件: {}", display_path_for_user(path))};
      }

      jpeg_decompress_struct cinfo{};
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324 4611)
#endif
      jpeg_detail::ErrorManager error{};
      cinfo.err = jpeg_std_error(&error.pub);
      error.pub.error_exit = jpeg_detail::error_exit;
      jpeg_detail::DecompressCleanup cleanup{.cinfo = &cinfo};
      if (setjmp(error.jump) != 0) {
        return std::unexpected{std::format("JPEG 读取尺寸失败: {}", error.message)};
      }
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
      jpeg_create_decompress(&cinfo);
      cleanup.created = true;
      if (auto metadata_budget = jpeg_detail::validate_saved_metadata_budget_from_file(
              input.file, display_path_for_user(path), false);
          !metadata_budget) {
        return std::unexpected{metadata_budget.error()};
      }
      jpeg_stdio_src(&cinfo, input.file);
      jpeg_save_markers(&cinfo, JPEG_APP0 + 2, 0xFFFFu);
      if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        return std::unexpected{std::format("JPEG 文件头无效: {}", display_path_for_user(path))};
      }
      if (cinfo.image_width > JPEG_MAX_DIMENSION || cinfo.image_height > JPEG_MAX_DIMENSION) {
        return std::unexpected{std::format("JPEG 尺寸超过 libjpeg-turbo 上限: {}", display_path_for_user(path))};
      }
      auto dimensions = decoder_common::make_image_dimensions_checked(cinfo.image_width,
                                                           cinfo.image_height, "JPEG");
      if (dimensions) dimensions->opaque_sdr8_yuv420 = jpeg_detail::opaque_sdr8_yuv420_header(cinfo);
      return dimensions;
    } catch (const std::bad_alloc&) {
      return std::unexpected{"JPEG 尺寸探测内存不足。"};
    } catch (const std::length_error&) {
      return std::unexpected{"JPEG 尺寸探测数据超过运行时限制。"};
    } catch (const std::filesystem::filesystem_error&) {
      return std::unexpected{"JPEG 尺寸探测文件系统访问失败。"};
    }
  }

  std::expected<ImageDecodeResult, std::string> decode(const fs::path& path) const override {
    try {
      auto bytes = decoder_common::read_file_bytes(path, "JPEG");
      if (!bytes) {
        return std::unexpected{bytes.error()};
      }

      std::unique_ptr<jpeg_detail::DecodeContext> context;
      try {
        context = std::make_unique<jpeg_detail::DecodeContext>();
      } catch (const std::bad_alloc&) {
        return std::unexpected{"JPEG decoder 内存不足。"};
      }

      jpeg_decompress_struct cinfo{};
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324 4611)
#endif
      jpeg_detail::ErrorManager error{};
      cinfo.err = jpeg_std_error(&error.pub);
      error.pub.error_exit = jpeg_detail::error_exit;
      jpeg_detail::DecompressCleanup cleanup{.cinfo = &cinfo};
      if (setjmp(error.jump) != 0) {
        return std::unexpected{std::format("JPEG 解码失败: {}", error.message)};
      }
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

      if (bytes->size() > static_cast<std::size_t>(std::numeric_limits<unsigned long>::max())) {
        return std::unexpected{"JPEG 文件超过 libjpeg API 限制。"};
      }
      jpeg_create_decompress(&cinfo);
      cleanup.created = true;
      if (auto metadata_budget = jpeg_detail::validate_saved_metadata_budget_from_memory(
              *bytes, display_path_for_user(path), true);
          !metadata_budget) {
        return std::unexpected{metadata_budget.error()};
      }
      jpeg_mem_src(&cinfo, reinterpret_cast<const unsigned char*>(bytes->data()),
                   static_cast<unsigned long>(bytes->size()));
      jpeg_save_markers(&cinfo, JPEG_APP0 + 1, 0xFFFFu);
      jpeg_save_markers(&cinfo, JPEG_APP0 + 2, 0xFFFFu);
      if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        return std::unexpected{std::format("JPEG 文件头无效: {}", display_path_for_user(path))};
      }
      const auto source_info = jpeg_detail::source_info_from_header(cinfo);
      cinfo.out_color_space = JCS_EXT_RGBA;
      jpeg_start_decompress(&cinfo);
      if (cinfo.output_width == 0 || cinfo.output_height == 0 || cinfo.output_components != 4) {
        return std::unexpected{std::format("JPEG 输出格式无效: {}", display_path_for_user(path))};
      }
      const auto width = static_cast<std::size_t>(cinfo.output_width);
      const auto height = static_cast<std::size_t>(cinfo.output_height);
      std::size_t stride = 0;
      {
        const auto checked_stride = decoder_common::checked_rgba_stride(width, "JPEG decoder");
        if (!checked_stride) {
          return std::unexpected{checked_stride.error()};
        }
        stride = *checked_stride;
      }
      {
        const auto byte_count = decoder_common::checked_image_bytes(stride, height, "JPEG decoder");
        if (!byte_count) {
          return std::unexpected{byte_count.error()};
        }
        auto rgba = decoder_common::make_byte_buffer(*byte_count, "JPEG decoder");
        if (!rgba) {
          return std::unexpected{rgba.error()};
        }
        context->rgba = std::move(*rgba);
      }
      while (cinfo.output_scanline < cinfo.output_height) {
        auto* row = reinterpret_cast<JSAMPROW>(context->rgba.data() + cinfo.output_scanline * stride);
        if (jpeg_read_scanlines(&cinfo, &row, 1) != 1) {
          return std::unexpected{std::format("JPEG 扫描行读取未推进: {}", display_path_for_user(path))};
        }
      }
      jpeg_finish_decompress(&cinfo);

      {
        auto icc_profile = jpeg_detail::copy_icc_profile(cinfo);
        if (!icc_profile) {
          return std::unexpected{icc_profile.error()};
        }
        context->icc_profile = std::move(*icc_profile);
      }

      auto image = decoder_common::make_rgba_image(width, height, std::move(context->rgba),
                                                   AlphaMode::none, "JPEG decoder", source_info);
      if (!image) {
        return std::unexpected{image.error()};
      }
      if (!context->icc_profile.empty()) {
        image->metadata.push_back(MetadataBlock{.kind = MetadataKind::icc,
                                                .bytes = std::move(context->icc_profile)});
      }
      if (auto copied = jpeg_detail::copy_app1_metadata(*image, cinfo); !copied) {
        return std::unexpected{copied.error()};
      }
      return ImageDecodeResult{.image = std::move(*image), .decoder_id = "libjpeg-turbo"};
    } catch (const std::bad_alloc&) {
      return std::unexpected{"JPEG 解码内存不足。"};
    } catch (const std::length_error&) {
      return std::unexpected{"JPEG 解码数据超过运行时限制。"};
    } catch (const std::filesystem::filesystem_error&) {
      return std::unexpected{"JPEG 解码文件系统访问失败。"};
    }
  }
};

}  // namespace awj
