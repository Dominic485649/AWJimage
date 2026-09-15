#include "studio_config.h"

#include "studio_json.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

import awj.core;
import awj.encoding_defaults;
import awj.studio_defaults;

namespace awj::studio {

std::string menu_config_key(std::string_view prefix, std::string_view name) {
  return std::format("menu_{}_{}", prefix, name);
}

std::filesystem::path studio_config_path() {
  if (auto directory = awj::executable_directory()) {
    return *directory /
           awj::wide_from_utf8(
               std::string{awj::studio_defaults::config_file_name});
  }
  return {};
}

std::string json_escape(std::string_view value) {
  std::string out;
  out.reserve(value.size() + 8);
  for (const char ch : value) {
    switch (ch) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out.push_back(ch);
        break;
    }
  }
  return out;
}

void append_json_config_line(std::vector<std::string>& lines,
                             std::string_view key, std::string value) {
  lines.push_back(std::format("  \"{}\": {}", key, value));
}

// ---------------------------------------------------------------------------
// 原子写文件：同目录临时文件 → 刷新磁盘 → 原子替换。
//
// 三步缺一不可：
//   * 临时文件必须和目标同目录，否则跨卷时替换退化成「复制+删除」，不再原子；
//   * 替换前必须把数据刷到盘上，否则崩溃后可能得到一个大小正确但内容为零的文件
//     （元数据先于数据落盘）；
//   * 替换本身要用平台的原子接口，让读取方要么看到旧内容、要么看到新内容。
//
// 失败时清理临时文件，绝不动原文件——写失败的正确结果是「配置没变」，
// 而不是「配置没了」。
//
// 这里是 main.cpp 的 Windows 半区；Linux Studio 有自己的同名实现，见文件后半段。
// ---------------------------------------------------------------------------
std::expected<void, std::string> write_file_atomically(
    const std::filesystem::path& path, std::string_view content) {
  std::error_code ec;
  auto temp_path = path;
  temp_path += L".tmp";

  // 上一次失败可能留下残留，先清掉；这里失败不致命，创建时还会再报一次。
  std::filesystem::remove(temp_path, ec);

  {
    const HANDLE file = CreateFileW(
        temp_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
      return std::unexpected{"无法创建配置临时文件。"};
    }
    struct HandleCloser {
      HANDLE handle{};
      ~HandleCloser() {
        if (handle != INVALID_HANDLE_VALUE) {
          CloseHandle(handle);
        }
      }
    } closer{file};

    std::size_t written_total = 0;
    while (written_total < content.size()) {
      const auto chunk = static_cast<DWORD>(
          std::min<std::size_t>(content.size() - written_total, 1u << 20));
      DWORD written = 0;
      if (WriteFile(file, content.data() + written_total, chunk, &written,
                    nullptr) == FALSE ||
          written == 0) {
        std::filesystem::remove(temp_path, ec);
        return std::unexpected{"写入配置临时文件失败。"};
      }
      written_total += written;
    }
    // 元数据可能先于数据落盘，必须显式 flush 才能保证替换后的文件内容完整。
    if (FlushFileBuffers(file) == FALSE) {
      std::filesystem::remove(temp_path, ec);
      return std::unexpected{"刷新配置临时文件到磁盘失败。"};
    }
  }

  // MoveFileEx 的替换在同卷上是原子的；WRITE_THROUGH 让目录项也落盘。
  if (MoveFileExW(temp_path.c_str(), path.c_str(),
                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
    std::filesystem::remove(temp_path, ec);
    return std::unexpected{"替换程序同目录配置文件失败。"};
  }
  return {};
}

std::expected<void, std::string> write_studio_config_file(
    const StudioConfigSnapshot& current,
    const StudioConfigSnapshot& defaults) try {
  const auto path = studio_config_path();
  if (path.empty()) {
    return std::unexpected{"无法定位程序同目录配置文件路径。"};
  }
  std::vector<std::string> lines;
  const auto add_int = [&](std::string_view key, int value, int fallback) {
    if (value != fallback) {
      append_json_config_line(lines, key, std::format("{}", value));
    }
  };
  const auto add_bool = [&](std::string_view key, bool value, bool fallback) {
    if (value != fallback) {
      append_json_config_line(lines, key, value ? "true" : "false");
    }
  };
  const auto add_string = [&](std::string_view key, const std::string& value,
                              const std::string& fallback) {
    if (value != fallback) {
      append_json_config_line(lines, key,
                              std::format("\"{}\"", json_escape(value)));
    }
  };
  // Unix 时间戳和 manifest 序号都会超出 int，必须单独走 64 位。
  const auto add_int64 = [&](std::string_view key, std::int64_t value,
                             std::int64_t fallback) {
    if (value != fallback) {
      append_json_config_line(lines, key, std::format("{}", value));
    }
  };

  add_int("theme_index", current.theme_index, defaults.theme_index);
  add_int("language_index", current.language_index, defaults.language_index);
  add_string("ui_font_family", current.ui_font_family, defaults.ui_font_family);

  add_bool("allow_wic_fallback", current.allow_wic_fallback,
           defaults.allow_wic_fallback);
  add_bool("visual_quality_gpu", current.visual_quality_gpu,
           defaults.visual_quality_gpu);
  add_bool("visual_quality_fallback", current.visual_quality_fallback,
           defaults.visual_quality_fallback);

  add_string("update_channel", current.update_channel,
             defaults.update_channel);
  add_bool("show_update_changelog", current.show_update_changelog,
           defaults.show_update_changelog);
  add_bool("hide_update_changelog_after_exit",
           current.hide_update_changelog_after_exit,
           defaults.hide_update_changelog_after_exit);
  add_bool("show_update_changelog_after_update",
           current.show_update_changelog_after_update,
           defaults.show_update_changelog_after_update);
  add_string("last_changelog_exit_version", current.last_changelog_exit_version,
             defaults.last_changelog_exit_version);
  add_int64("last_successful_update_check_at",
            current.last_successful_update_check_at,
            defaults.last_successful_update_check_at);
  add_int64("last_verified_manifest_sequence",
            current.last_verified_manifest_sequence,
            defaults.last_verified_manifest_sequence);
  add_int64("last_verified_manifest_v2_sequence",
            current.last_verified_manifest_v2_sequence,
            defaults.last_verified_manifest_v2_sequence);
  add_string("pending_update_version", current.pending_update_version,
             defaults.pending_update_version);
  add_string("pending_update_channel", current.pending_update_channel,
             defaults.pending_update_channel);
  add_string("pending_update_release_url", current.pending_update_release_url,
             defaults.pending_update_release_url);
  add_string("pending_update_published_at",
             current.pending_update_published_at,
             defaults.pending_update_published_at);
  add_string("pending_update_changelog_zh_cn",
             current.pending_update_changelog_zh_cn,
             defaults.pending_update_changelog_zh_cn);
  add_string("pending_update_changelog_en",
             current.pending_update_changelog_en,
             defaults.pending_update_changelog_en);
  add_string("update_manifest_raw", current.update_manifest_raw,
             defaults.update_manifest_raw);
  add_string("update_manifest_signature", current.update_manifest_signature,
             defaults.update_manifest_signature);
  add_string("update_manifest_v2_raw", current.update_manifest_v2_raw,
             defaults.update_manifest_v2_raw);
  add_string("update_manifest_v2_signature", current.update_manifest_v2_signature,
             defaults.update_manifest_v2_signature);
  add_string("update_keyring_raw", current.update_keyring_raw,
             defaults.update_keyring_raw);
  add_string("update_keyring_signature", current.update_keyring_signature,
             defaults.update_keyring_signature);

  for (std::size_t i = 0; i < current.menu_params.size(); ++i) {
    const auto prefix = menu_config_prefixes[i];
    const auto& value = current.menu_params[i];
    const auto& fallback = defaults.menu_params[i];
    add_string(menu_config_key(prefix, "quality_text"), value.quality_text, fallback.quality_text);
    add_string(menu_config_key(prefix, "bit_depth_text"), value.bit_depth_text, fallback.bit_depth_text);
    add_string(menu_config_key(prefix, "speed_text"), value.speed_text, fallback.speed_text);
    add_int(menu_config_key(prefix, "avif_encoder_index"), value.avif_encoder_index == 1 ? 2 : value.avif_encoder_index,
            fallback.avif_encoder_index == 1 ? 2 : fallback.avif_encoder_index);
    add_int(menu_config_key(prefix, "avif_color_representation_index"), value.avif_color_representation_index, fallback.avif_color_representation_index);
    add_int(menu_config_key(prefix, "chroma_index"), value.chroma_index, fallback.chroma_index);
    add_int(menu_config_key(prefix, "alpha_policy_index"), value.alpha_policy_index, fallback.alpha_policy_index);
    add_int(menu_config_key(prefix, "jpegli_progressive_index"), value.jpegli_progressive_index, fallback.jpegli_progressive_index);
    add_bool(menu_config_key(prefix, "jpegli_optimize_huffman"), value.jpegli_optimize_huffman, fallback.jpegli_optimize_huffman);
    add_bool(menu_config_key(prefix, "jpegli_xyb"), value.jpegli_xyb, fallback.jpegli_xyb);
    add_bool(menu_config_key(prefix, "jxl_jpeg_lossless"), value.jxl_jpeg_lossless, fallback.jxl_jpeg_lossless);
    add_bool(menu_config_key(prefix, "strip_metadata"), value.strip_metadata, fallback.strip_metadata);
    add_bool(menu_config_key(prefix, "allow_wic_fallback"), value.allow_wic_fallback, fallback.allow_wic_fallback);
    add_bool(menu_config_key(prefix, "close_on_finish"), value.close_on_finish, fallback.close_on_finish);
    add_bool(menu_config_key(prefix, "install_avif_png_command"), value.install_avif_png_command, fallback.install_avif_png_command);
    add_int(menu_config_key(prefix, "size_limit_index"), value.size_limit_index, fallback.size_limit_index);
    add_string(menu_config_key(prefix, "max_width_text"), value.max_width_text, fallback.max_width_text);
    add_string(menu_config_key(prefix, "max_height_text"), value.max_height_text, fallback.max_height_text);
    add_string(menu_config_key(prefix, "max_long_edge_text"), value.max_long_edge_text, fallback.max_long_edge_text);
    add_string(menu_config_key(prefix, "max_short_edge_text"), value.max_short_edge_text, fallback.max_short_edge_text);
    add_string(menu_config_key(prefix, "scale_percent_text"), value.scale_percent_text, fallback.scale_percent_text);
  }

  // 先在内存里拼出完整内容，再原子落盘。直接 truncate 写目标文件的话，进程在
  // 写到一半时被杀（或断电）会留下一个被截断的 AWJ.jsonc —— 下次启动解析失败，
  // 用户的全部设置一起丢。更新流程会往同一份配置里写待更新状态，出错代价更高。
  std::string content;
  content += "{\n";
  content +=
      "  // AWJ Studio runtime config. Only values that differ from "
      "built-in defaults are written.\n";
  for (std::size_t i = 0; i < lines.size(); ++i) {
    content += lines[i];
    if (i + 1 < lines.size()) {
      content += ',';
    }
    content += '\n';
  }
  content += "}\n";

  return write_file_atomically(path, content);
} catch (const std::bad_alloc&) {
  return std::unexpected{"写入 Studio 配置时内存不足。"};
} catch (const std::length_error&) {
  return std::unexpected{"写入 Studio 配置时数据超过运行时限制。"};
} catch (const std::filesystem::filesystem_error&) {
  return std::unexpected{"写入 Studio 配置时发生文件系统错误。"};
}

}  // namespace awj::studio
