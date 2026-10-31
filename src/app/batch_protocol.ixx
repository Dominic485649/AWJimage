module;
#include "../compat/stdlib_compat.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifdef _WIN32
#include <windows.h>
#endif
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <new>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>
export module awj.batch_protocol;
import awj.config;
import awj.encoding_defaults;
import awj.core;
import awj.large_image_plan;
export namespace awj {
struct BatchLargeImageItem {
  ImageFile file{};
  ImageDimensions dimensions{};
  LargeImageDecision decision{};
};

enum class BatchEventKind {
  message,
  warning,
  item_started,
  item_finished,
  large_image_queued,
  summary
};

struct BatchSummary {
  int ok_count{};
  int failed_count{};
  int canceled_count{};
  int large_image_queued_count{};
  std::uintmax_t original_total{};
  std::uintmax_t output_total{};
  bool canceled{};
  int exit_code{};
};

struct BatchProgress {
  BatchEventKind kind{BatchEventKind::message};
  std::size_t completed{};
  std::size_t total{};
  EncodeResult result{};
  BatchLargeImageItem large_image{};
  BatchSummary summary{};
  std::string text{};
};

struct StudioQueueManifest {
  std::vector<ImageFile> files{};
};

namespace pipeline_detail {

constexpr std::array<unsigned char, 8> studio_queue_manifest_magic{
    'A', 'W', 'J', 'S', 'Q', 'M', 'F', 0};
constexpr std::uint32_t studio_queue_manifest_version = 1;
constexpr std::uintmax_t max_studio_queue_manifest_bytes =
    64ull * 1024ull * 1024ull;
constexpr std::uint64_t max_studio_queue_manifest_files = 1'000'000;
constexpr std::uint32_t max_studio_queue_manifest_text_bytes = 1024 * 1024;
constexpr std::uintmax_t studio_queue_manifest_header_bytes = 8 + 4 + 8;
constexpr std::uintmax_t studio_queue_manifest_record_min_bytes =
    8 + 8 + 4 + 11 * 4;

void manifest_write_u32(std::ostream& output, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    output.put(static_cast<char>((value >> shift) & 0xffu));
  }
}

void manifest_write_u64(std::ostream& output, std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    output.put(static_cast<char>((value >> shift) & 0xffu));
  }
}

bool manifest_read_u32(std::istream& input, std::uint32_t& value) {
  value = 0;
  for (int shift = 0; shift < 32; shift += 8) {
    const auto byte = input.get();
    if (byte == std::char_traits<char>::eof()) {
      return false;
    }
    value |= static_cast<std::uint32_t>(static_cast<unsigned char>(byte))
             << shift;
  }
  return true;
}

bool manifest_read_u64(std::istream& input, std::uint64_t& value) {
  value = 0;
  for (int shift = 0; shift < 64; shift += 8) {
    const auto byte = input.get();
    if (byte == std::char_traits<char>::eof()) {
      return false;
    }
    value |= static_cast<std::uint64_t>(static_cast<unsigned char>(byte))
             << shift;
  }
  return true;
}

awj::compat::expected<void, std::string> manifest_write_text(
    std::ostream& output, std::string_view value) {
  if (value.size() > max_studio_queue_manifest_text_bytes ||
      value.find('\0') != std::string_view::npos) {
    return awj::compat::unexpected{"Studio 队列 manifest 字段无效或过长。"};
  }
  manifest_write_u32(output, static_cast<std::uint32_t>(value.size()));
  output.write(value.data(), static_cast<std::streamsize>(value.size()));
  if (!output) {
    return awj::compat::unexpected{"写入 Studio 队列 manifest 失败。"};
  }
  return {};
}

awj::compat::expected<std::string, std::string> manifest_read_text(
    std::istream& input) {
  std::uint32_t size = 0;
  if (!manifest_read_u32(input, size) ||
      size > max_studio_queue_manifest_text_bytes) {
    return awj::compat::unexpected{"Studio 队列 manifest 字段长度无效。"};
  }
  std::string value(size, '\0');
  input.read(value.data(), static_cast<std::streamsize>(value.size()));
  if (input.gcount() != static_cast<std::streamsize>(value.size()) ||
      value.find('\0') != std::string::npos) {
    return awj::compat::unexpected{"Studio 队列 manifest 字段不完整或无效。"};
  }
  return value;
}

awj::compat::expected<std::wstring, std::string> manifest_wide_from_utf8(
    std::string_view value) {
#ifdef _WIN32
  if (value.empty()) {
    return std::wstring{};
  }
  if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return awj::compat::unexpected{"Studio 队列 manifest UTF-8 字段过长。"};
  }
  const int size = static_cast<int>(value.size());
  const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                           value.data(), size, nullptr, 0);
  if (required <= 0) {
    return awj::compat::unexpected{"Studio 队列 manifest 包含无效 UTF-8。"};
  }
  std::wstring result(static_cast<std::size_t>(required), L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), size,
                          result.data(), required) != required) {
    return awj::compat::unexpected{"Studio 队列 manifest UTF-8 转换失败。"};
  }
  return result;
#else
  return wide_from_utf8(value);
#endif
}

awj::compat::expected<fs::path, std::string> manifest_path_from_utf8(
    std::string_view value) {
#ifdef _WIN32
  auto wide = manifest_wide_from_utf8(value);
  if (!wide) {
    return awj::compat::unexpected{wide.error()};
  }
  return fs::path{std::move(*wide)};
#else
  return fs::path{std::string{value}};
#endif
}

bool safe_manifest_relative_path(const fs::path& path) {
  if (path.is_absolute() || path.has_root_name() || path.has_root_directory()) {
    return false;
  }
  return awj::compat::ranges::none_of(path, [](const fs::path& part) {
    return part == fs::path{".."};
  });
}

awj::compat::expected<void, std::string> validate_manifest_paths_for_config(
    const AppConfig& cfg, std::span<const ImageFile> files) {
  std::error_code ec;
  const auto output_root = fs::weakly_canonical(output_dir_for(cfg), ec);
  if (ec || output_root.empty()) {
    return awj::compat::unexpected{"Studio 队列 manifest 输出目录无效。"};
  }

  std::unordered_set<PathText> input_keys;
  input_keys.reserve(files.size());
  for (const auto& file : files) {
    const auto input = fs::weakly_canonical(file.path, ec);
    if (ec || input.empty()) {
      return awj::compat::unexpected{"Studio 队列 manifest 输入路径无效。"};
    }
    input_keys.insert(normalized_lower_path_key(input));
  }
  const auto expected_extension =
      normalized_lower_path_key(fs::path{output_extension_for(cfg)});

  for (const auto& file : files) {
    if (!file.path.is_absolute() ||
        !safe_manifest_relative_path(file.relative_dir)) {
      return awj::compat::unexpected{"Studio 队列 manifest 输入或相对路径无效。"};
    }
    const auto expected_disambiguator = source_extension_disambiguator(file.path);
    if ((file.extension_disambiguated &&
         file.source_extension_disambiguator != expected_disambiguator) ||
        (!file.extension_disambiguated &&
         !file.source_extension_disambiguator.empty()) ||
        (!file.output_path_resolved && !file.resolved_output_path.empty())) {
      return awj::compat::unexpected{"Studio 队列 manifest 输出命名字段无效。"};
    }
    ec.clear();
    const auto output = fs::weakly_canonical(output_path_for(cfg, file), ec);
    if (ec || output.empty()) {
      return awj::compat::unexpected{"Studio 队列 manifest 输出路径无效。"};
    }
    const auto relative = output.lexically_relative(output_root);
    if (relative.empty() || relative == fs::path{"."} ||
        !safe_manifest_relative_path(relative) ||
        input_keys.contains(normalized_lower_path_key(output)) ||
        !normalized_lower_path_key(output.filename())
             .ends_with(expected_extension)) {
      return awj::compat::unexpected{
          "Studio 队列 manifest 输出路径越界、扩展名无效或会覆盖输入。"};
    }
  }
  return {};
}

}  // namespace pipeline_detail

awj::compat::expected<void, std::string> write_studio_queue_manifest(
    const fs::path& path, std::span<const ImageFile> files) {
  try {
    if (files.empty() ||
        files.size() > pipeline_detail::max_studio_queue_manifest_files) {
      return awj::compat::unexpected{"Studio 队列 manifest 图片数量无效。"};
    }
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    if (!output) {
      return awj::compat::unexpected{"无法创建 Studio 队列 manifest。"};
    }
    output.write(reinterpret_cast<const char*>(
                     pipeline_detail::studio_queue_manifest_magic.data()),
                 static_cast<std::streamsize>(
                     pipeline_detail::studio_queue_manifest_magic.size()));
    pipeline_detail::manifest_write_u32(
        output, pipeline_detail::studio_queue_manifest_version);
    pipeline_detail::manifest_write_u64(output,
                                        static_cast<std::uint64_t>(files.size()));

    std::uintmax_t manifest_bytes =
        pipeline_detail::studio_queue_manifest_header_bytes;
    for (std::size_t index = 0; index < files.size(); ++index) {
      const auto& file = files[index];
      if (file.index != index || !file.path.is_absolute() ||
          !pipeline_detail::safe_manifest_relative_path(file.relative_dir) ||
          file.bytes > encoding_defaults::effective_max_input_file_bytes() ||
          (file.output_path_resolved &&
           !file.resolved_output_path.is_absolute()) ||
          (!file.output_path_resolved &&
           !file.resolved_output_path.empty())) {
        return awj::compat::unexpected{"Studio 队列 manifest 图片记录无效。"};
      }
      if (manifest_bytes >
          pipeline_detail::max_studio_queue_manifest_bytes -
              pipeline_detail::studio_queue_manifest_record_min_bytes) {
        return awj::compat::unexpected{"Studio 队列 manifest 超过 64 MiB。"};
      }
      manifest_bytes += pipeline_detail::studio_queue_manifest_record_min_bytes;
      pipeline_detail::manifest_write_u64(output, file.index);
      pipeline_detail::manifest_write_u64(output, file.bytes);
      const std::uint32_t flags =
          (file.extension_disambiguated ? 1u : 0u) |
          (file.output_path_resolved ? 2u : 0u);
      pipeline_detail::manifest_write_u32(output, flags);
      const std::array<std::string, 11> fields{
          path_to_utf8(file.path),
          path_to_utf8(file.relative_dir),
          utf8_from_wide(file.source_extension_disambiguator),
          utf8_from_wide(file.date_token),
          utf8_from_wide(file.time_token),
          utf8_from_wide(file.datetime_token),
          utf8_from_wide(file.unix_token),
          utf8_from_wide(file.random_token),
          utf8_from_wide(file.hash_token),
          utf8_from_wide(file.sha256_token),
          path_to_utf8(file.resolved_output_path)};
      for (const auto& field : fields) {
        if (field.size() >
            pipeline_detail::max_studio_queue_manifest_bytes -
                manifest_bytes) {
          return awj::compat::unexpected{"Studio 队列 manifest 超过 64 MiB。"};
        }
        manifest_bytes += field.size();
        if (auto written = pipeline_detail::manifest_write_text(output, field);
            !written) {
          return written;
        }
      }
    }
    output.flush();
    const auto size = output.tellp();
    if (!output || size < 0 ||
        static_cast<std::uintmax_t>(size) >
            pipeline_detail::max_studio_queue_manifest_bytes) {
      return awj::compat::unexpected{"Studio 队列 manifest 写入失败或超过 64 MiB。"};
    }
    return {};
  } catch (const std::bad_alloc&) {
    return awj::compat::unexpected{"Studio 队列 manifest 内存不足。"};
  } catch (const std::length_error&) {
    return awj::compat::unexpected{"Studio 队列 manifest 数据超过运行时限制。"};
  } catch (const std::filesystem::filesystem_error&) {
    return awj::compat::unexpected{"Studio 队列 manifest 文件系统访问失败。"};
  }
}

awj::compat::expected<StudioQueueManifest, std::string> read_studio_queue_manifest(
    const fs::path& path) {
  try {
    std::error_code ec;
    const auto manifest_bytes = fs::file_size(path, ec);
    if (ec ||
        manifest_bytes < pipeline_detail::studio_queue_manifest_header_bytes ||
        manifest_bytes > pipeline_detail::max_studio_queue_manifest_bytes) {
      return awj::compat::unexpected{"Studio 队列 manifest 不存在、为空或超过 64 MiB。"};
    }
    std::ifstream input{path, std::ios::binary};
    if (!input) {
      return awj::compat::unexpected{"无法读取 Studio 队列 manifest。"};
    }
    std::array<unsigned char, 8> magic{};
    input.read(reinterpret_cast<char*>(magic.data()),
               static_cast<std::streamsize>(magic.size()));
    std::uint32_t version = 0;
    std::uint64_t file_count = 0;
    if (magic != pipeline_detail::studio_queue_manifest_magic ||
        !pipeline_detail::manifest_read_u32(input, version) ||
        version != pipeline_detail::studio_queue_manifest_version ||
        !pipeline_detail::manifest_read_u64(input, file_count) ||
        file_count == 0 ||
        file_count > pipeline_detail::max_studio_queue_manifest_files) {
      return awj::compat::unexpected{"Studio 队列 manifest 头部或版本无效。"};
    }
    const auto max_records_by_size =
        (manifest_bytes - pipeline_detail::studio_queue_manifest_header_bytes) /
        pipeline_detail::studio_queue_manifest_record_min_bytes;
    if (file_count > max_records_by_size) {
      return awj::compat::unexpected{"Studio 队列 manifest 记录数量与文件大小不符。"};
    }

    StudioQueueManifest manifest;
    for (std::uint64_t index = 0; index < file_count; ++index) {
      std::uint64_t stored_index = 0;
      std::uint64_t bytes = 0;
      std::uint32_t flags = 0;
      if (!pipeline_detail::manifest_read_u64(input, stored_index) ||
          stored_index != index ||
          !pipeline_detail::manifest_read_u64(input, bytes) ||
          bytes > encoding_defaults::effective_max_input_file_bytes() ||
          !pipeline_detail::manifest_read_u32(input, flags) ||
          (flags & ~3u) != 0) {
        return awj::compat::unexpected{"Studio 队列 manifest 图片记录头无效。"};
      }
      std::array<std::string, 11> fields;
      for (auto& field : fields) {
        auto read = pipeline_detail::manifest_read_text(input);
        if (!read) {
          return awj::compat::unexpected{read.error()};
        }
        field = std::move(*read);
      }
      auto input_path = pipeline_detail::manifest_path_from_utf8(fields[0]);
      auto relative_dir = pipeline_detail::manifest_path_from_utf8(fields[1]);
      auto resolved_output = pipeline_detail::manifest_path_from_utf8(fields[10]);
      if (!input_path || !relative_dir || !resolved_output ||
          !input_path->is_absolute() ||
          !pipeline_detail::safe_manifest_relative_path(*relative_dir) ||
          ((flags & 2u) != 0 && !resolved_output->is_absolute()) ||
          ((flags & 2u) == 0 && !resolved_output->empty())) {
        return awj::compat::unexpected{"Studio 队列 manifest 图片路径无效。"};
      }
      std::array<std::wstring, 8> wide_fields;
      for (std::size_t field = 0; field < wide_fields.size(); ++field) {
        auto converted =
            pipeline_detail::manifest_wide_from_utf8(fields[field + 2]);
        if (!converted) {
          return awj::compat::unexpected{converted.error()};
        }
        wide_fields[field] = std::move(*converted);
      }
      const bool extension_disambiguated = (flags & 1u) != 0;
      if ((extension_disambiguated &&
           wide_fields[0] != source_extension_disambiguator(*input_path)) ||
          (!extension_disambiguated && !wide_fields[0].empty())) {
        return awj::compat::unexpected{
            "Studio 队列 manifest 源扩展名消歧字段无效。"};
      }
      manifest.files.push_back(ImageFile{
          .index = static_cast<std::size_t>(stored_index),
          .path = std::move(*input_path),
          .relative_dir = std::move(*relative_dir),
          .source_extension_disambiguator = std::move(wide_fields[0]),
          .bytes = static_cast<std::uintmax_t>(bytes),
          .date_token = std::move(wide_fields[1]),
          .time_token = std::move(wide_fields[2]),
          .datetime_token = std::move(wide_fields[3]),
          .unix_token = std::move(wide_fields[4]),
          .random_token = std::move(wide_fields[5]),
          .hash_token = std::move(wide_fields[6]),
          .sha256_token = std::move(wide_fields[7]),
          .extension_disambiguated = extension_disambiguated,
          .resolved_output_path = std::move(*resolved_output),
          .output_path_resolved = (flags & 2u) != 0});
    }
    if (input.peek() != std::char_traits<char>::eof()) {
      return awj::compat::unexpected{"Studio 队列 manifest 包含多余数据。"};
    }
    return manifest;
  } catch (const std::bad_alloc&) {
    return awj::compat::unexpected{"读取 Studio 队列 manifest 时内存不足。"};
  } catch (const std::length_error&) {
    return awj::compat::unexpected{"Studio 队列 manifest 数据超过运行时限制。"};
  } catch (const std::filesystem::filesystem_error&) {
    return awj::compat::unexpected{"读取 Studio 队列 manifest 时文件系统访问失败。"};
  }
}

using ProgressCallback = std::function<void(const BatchProgress&)>;


} // namespace awj
