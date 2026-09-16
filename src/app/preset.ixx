module;

#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>
#endif

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

export module awj.preset;

import awj.config;
import awj.encoding_defaults;

export namespace awj {

inline constexpr int user_preset_schema = 1;

struct PresetFormat {
  int quality{};
  std::optional<int> visual_quality{};
  std::optional<int> bit_depth{};
  std::optional<int> speed{};
  AvifEncoderMode avif_encoder{AvifEncoderMode::automatic};
  AvifColorRepresentation avif_color_representation{
      AvifColorRepresentation::yuv};
  ChromaMode chroma_mode{ChromaMode::auto_keep};
  AlphaModePolicy alpha_policy{AlphaModePolicy::automatic};
  int jpegli_progressive_level{2};
  bool jpegli_optimize_huffman{true};
  bool jpegli_xyb{};
  bool jxl_jpeg_lossless{true};
  int max_jobs{default_max_jobs()};
  std::uint64_t memory_limit_bytes{};
  ImageSizeLimit image_size_limit{};

};

struct UserPreset {
  int schema{user_preset_schema};
  std::string name{};
  std::string description{};
  std::array<PresetFormat, 5> formats{};
  std::filesystem::path source_path{};
  bool shell_menu{};

};

struct PresetCatalog {
  std::vector<UserPreset> presets{};
  // 非法文件不会悄悄作为默认值参与选择；UI 可直接呈现具体原因。
  std::vector<std::string> errors{};
};

namespace preset_detail {

using Json = nlohmann::ordered_json;
namespace fs = std::filesystem;

constexpr std::array<std::string_view, 5> format_keys{
    "avif", "webp", "jxl", "jpgli", "png"};

std::size_t format_slot(OutputFormat format) noexcept {
  switch (format) {
    case OutputFormat::avif:
      return 0;
    case OutputFormat::webp:
      return 1;
    case OutputFormat::jxl:
      return 2;
    case OutputFormat::jpgli:
      return 3;
    case OutputFormat::png:
    default:
      return 4;
  }
}

OutputFormat format_from_slot(std::size_t slot) noexcept {
  constexpr std::array formats{OutputFormat::avif, OutputFormat::webp,
                               OutputFormat::jxl, OutputFormat::jpgli,
                               OutputFormat::png};
  return formats[std::min(slot, formats.size() - 1)];
}

PresetFormat default_format(OutputFormat format) {
  PresetFormat value{};
  value.quality = default_quality_for(format);
  if (format == OutputFormat::avif || format == OutputFormat::webp ||
      format == OutputFormat::jxl) {
    value.speed = default_speed_for(format);
  }
  if (format == OutputFormat::webp || format == OutputFormat::jpgli) {
    value.bit_depth = encoding_defaults::default_webp_bit_depth;
  }
  return value;
}

std::array<PresetFormat, 5> default_formats() {
  std::array<PresetFormat, 5> formats{};
  for (std::size_t i = 0; i < formats.size(); ++i) {
    formats[i] = default_format(format_from_slot(i));
  }
  return formats;
}

std::string strip_jsonc_comments(std::string_view source) {
  std::string out;
  out.reserve(source.size());
  bool quoted = false;
  bool escaped = false;
  for (std::size_t i = 0; i < source.size(); ++i) {
    const char ch = source[i];
    if (quoted) {
      out.push_back(ch);
      if (escaped) {
        escaped = false;
      } else if (ch == '\\') {
        escaped = true;
      } else if (ch == '"') {
        quoted = false;
      }
      continue;
    }
    if (ch == '"') {
      quoted = true;
      out.push_back(ch);
    } else if (ch == '/' && i + 1 < source.size() && source[i + 1] == '/') {
      while (i < source.size() && source[i] != '\n') ++i;
      if (i < source.size()) out.push_back('\n');
    } else if (ch == '/' && i + 1 < source.size() && source[i + 1] == '*') {
      i += 2;
      while (i + 1 < source.size() &&
             !(source[i] == '*' && source[i + 1] == '/')) {
        out.push_back(source[i] == '\n' ? '\n' : ' ');
        ++i;
      }
      if (i + 1 < source.size()) ++i;
    } else {
      out.push_back(ch);
    }
  }
  return out;
}

std::expected<fs::path, std::string> running_executable_directory() {
#ifdef _WIN32
  std::vector<wchar_t> buffer(MAX_PATH, L'\0');
  while (true) {
    const DWORD written = GetModuleFileNameW(nullptr, buffer.data(),
                                              static_cast<DWORD>(buffer.size()));
    if (written == 0) {
      return std::unexpected{"无法定位当前可执行文件。"};
    }
    if (written + 1 < buffer.size()) {
      return fs::path{std::wstring_view{buffer.data(), written}}.parent_path();
    }
    if (buffer.size() > 1024u * 1024u) {
      return std::unexpected{"当前可执行文件路径过长。"};
    }
    buffer.resize(buffer.size() * 2, L'\0');
  }
#else
  std::vector<char> buffer(4096, '\0');
  while (true) {
    const auto written = ::readlink("/proc/self/exe", buffer.data(), buffer.size());
    if (written < 0) {
      return std::unexpected{"无法定位当前可执行文件。"};
    }
    if (static_cast<std::size_t>(written) < buffer.size()) {
      return fs::path{std::string_view{buffer.data(),
                                       static_cast<std::size_t>(written)}}
          .parent_path();
    }
    if (buffer.size() > 1024u * 1024u) {
      return std::unexpected{"当前可执行文件路径过长。"};
    }
    buffer.resize(buffer.size() * 2, '\0');
  }
#endif
}

std::string upper_ascii(std::string value) {
  std::ranges::transform(value, value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::toupper(ch));
  });
  return value;
}

std::string utf8_from_wide_for_preset(std::wstring_view value) {
  const auto utf8 = fs::path{std::wstring{value}}.u8string();
  return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}

std::expected<void, std::string> validate_display_name(std::string_view name) {
  if (name.empty() || name.size() > 240) {
    return std::unexpected{"预设名称不能为空且不能超过 240 字节。"};
  }
  if (std::ranges::any_of(name, [](unsigned char ch) {
        return ch < 0x20 || ch == 0x7f;
      })) {
    return std::unexpected{"预设名称不能包含控制字符。"};
  }
  if (name == "内置默认" || upper_ascii(std::string{name}) == "BUILT-IN DEFAULT") {
    return std::unexpected{"内置默认名称保留给程序，不能用于用户预设。"};
  }
  return {};
}

std::expected<std::string, std::string> safe_file_stem(std::string_view name) {
  if (auto valid = validate_display_name(name); !valid) {
    return std::unexpected{valid.error()};
  }
  std::string stem;
  stem.reserve(name.size());
  for (const unsigned char ch : name) {
    if (ch == '<' || ch == '>' || ch == ':' || ch == '"' || ch == '/' ||
        ch == '\\' || ch == '|' || ch == '?' || ch == '*' || ch < 0x20 ||
        ch == 0x7f) {
      stem.push_back('_');
    } else {
      stem.push_back(static_cast<char>(ch));
    }
  }
  while (!stem.empty() && (stem.back() == ' ' || stem.back() == '.')) {
    stem.pop_back();
  }
  if (stem.empty() || stem == "." || stem == "..") {
    return std::unexpected{"预设名称无法生成安全文件名。"};
  }
  const auto upper = upper_ascii(stem.substr(0, stem.find('.')));
  static constexpr std::array reserved{"CON", "PRN", "AUX", "NUL", "COM1",
                                        "COM2", "COM3", "COM4", "COM5", "COM6",
                                        "COM7", "COM8", "COM9", "LPT1", "LPT2",
                                        "LPT3", "LPT4", "LPT5", "LPT6", "LPT7",
                                        "LPT8", "LPT9"};
  if (std::ranges::find(reserved, upper) != reserved.end()) {
    return std::unexpected{"预设名称会生成 Windows 保留文件名。"};
  }
  if (stem.size() > 180) {
    std::size_t end = 180;
    while (end > 0 && (static_cast<unsigned char>(stem[end]) & 0xc0) == 0x80) --end;
    stem.resize(end);
  }
  return stem;
}

std::expected<void, std::string> require_type(const Json& value,
                                               bool condition,
                                               std::string_view key,
                                               std::string_view expected) {
  (void)value;
  if (!condition) {
    return std::unexpected{
        std::format("预设字段 {} 必须是{}。", key, expected)};
  }
  return {};
}

std::expected<void, std::string> load_int(const Json& object,
                                          std::string_view key, int minimum,
                                          int maximum, int& target) {
  const auto name = std::string{key};
  if (!object.contains(name)) return {};
  const auto& value = object.at(name);
  if (auto valid = require_type(value, value.is_number_integer(), key, "整数");
      !valid) return valid;
  const auto number = value.get<long long>();
  if (number < minimum || number > maximum) {
    return std::unexpected{std::format("预设字段 {} 范围必须在 {} 到 {}。", key,
                                       minimum, maximum)};
  }
  target = static_cast<int>(number);
  return {};
}

std::expected<void, std::string> load_optional_int(
    const Json& object, std::string_view key, int minimum, int maximum,
    std::optional<int>& target) {
  const auto name = std::string{key};
  if (!object.contains(name)) return {};
  const Json* value = &object.at(name);
  // 1.0.4 wrote optional scalars with Json{value}, which nlohmann::json
  // serializes as a one-item array.  Accept exactly that malformed legacy
  // shape once; all newly saved presets use a scalar/null below.
  if (value->is_array()) {
    if (value->size() != 1 ||
        (!value->front().is_null() && !value->front().is_number_integer())) {
      return std::unexpected{
          std::format("预设字段 {} 必须是整数或 null。", key)};
    }
    value = &value->front();
  }
  if (value->is_null()) {
    target.reset();
    return {};
  }
  if (auto valid = require_type(*value, value->is_number_integer(), key, "整数或 null");
      !valid) return valid;
  const auto number = value->get<long long>();
  if (number < minimum || number > maximum) {
    return std::unexpected{std::format("预设字段 {} 范围必须在 {} 到 {}。", key,
                                       minimum, maximum)};
  }
  target = static_cast<int>(number);
  return {};
}

std::expected<void, std::string> load_bool(const Json& object,
                                           std::string_view key, bool& target) {
  const auto name = std::string{key};
  if (!object.contains(name)) return {};
  const auto& value = object.at(name);
  if (auto valid = require_type(value, value.is_boolean(), key, "布尔值");
      !valid) return valid;
  target = value.get<bool>();
  return {};
}

std::expected<void, std::string> load_u64(const Json& object,
                                          std::string_view key,
                                          std::uint64_t maximum,
                                          std::uint64_t& target) {
  const auto name = std::string{key};
  if (!object.contains(name)) return {};
  const auto& value = object.at(name);
  if (auto valid = require_type(value, value.is_number_unsigned() || value.is_number_integer(),
                                key, "非负整数"); !valid) return valid;
  const auto number = value.get<long long>();
  if (number < 0 || static_cast<std::uint64_t>(number) > maximum) {
    return std::unexpected{std::format("预设字段 {} 超出范围。", key)};
  }
  target = static_cast<std::uint64_t>(number);
  return {};
}

std::string avif_encoder_value(AvifEncoderMode value) {
  return avif_encoder_mode_name(value);
}

std::expected<AvifEncoderMode, std::string> parse_avif_encoder_value(
    const Json& object, std::string_view key, AvifEncoderMode fallback) {
  const auto name = std::string{key};
  if (!object.contains(name)) return fallback;
  const auto& value = object.at(name);
  if (auto valid = require_type(value, value.is_string(), key, "字符串"); !valid) {
    return std::unexpected{valid.error()};
  }
  const auto text = value.get<std::string>();
  if (text == "auto") return AvifEncoderMode::automatic;
  if (text == "aom") return AvifEncoderMode::aom;
  return std::unexpected{"预设字段 avif_encoder 仅支持 auto/aom；旧编码器已移除，请修正该预设。"};
}

std::expected<ChromaMode, std::string> parse_chroma_value(
    const Json& object, std::string_view key, ChromaMode fallback) {
  const auto name = std::string{key};
  if (!object.contains(name)) return fallback;
  const auto& value = object.at(name);
  if (auto valid = require_type(value, value.is_string(), key, "字符串"); !valid) {
    return std::unexpected{valid.error()};
  }
  const auto text = value.get<std::string>();
  if (text == "auto") return ChromaMode::auto_keep;
  if (text == "444") return ChromaMode::yuv444;
  if (text == "422") return ChromaMode::yuv422;
  if (text == "420") return ChromaMode::yuv420;
  return std::unexpected{"预设字段 chroma 只支持 auto/444/422/420。"};
}

std::expected<AvifColorRepresentation, std::string>
parse_avif_color_representation_value(const Json& object, std::string_view key,
                                      AvifColorRepresentation fallback) {
  const auto name = std::string{key};
  if (!object.contains(name)) return fallback;
  const auto& value = object.at(name);
  if (auto valid = require_type(value, value.is_string(), key, "字符串"); !valid) {
    return std::unexpected{valid.error()};
  }
  const auto text = value.get<std::string>();
  if (text == "yuv") return AvifColorRepresentation::yuv;
  if (text == "source") return AvifColorRepresentation::source;
  if (text == "rgb") return AvifColorRepresentation::rgb_identity;
  return std::unexpected{
      "预设字段 avif_color_representation 只支持 yuv/source/rgb。"};
}

std::expected<AlphaModePolicy, std::string> parse_alpha_value(
    const Json& object, std::string_view key, AlphaModePolicy fallback) {
  const auto name = std::string{key};
  if (!object.contains(name)) return fallback;
  const auto& value = object.at(name);
  if (auto valid = require_type(value, value.is_string(), key, "字符串"); !valid) {
    return std::unexpected{valid.error()};
  }
  const auto text = value.get<std::string>();
  if (text == "force") return AlphaModePolicy::force;
  if (text == "auto") return AlphaModePolicy::automatic;
  if (text == "off") return AlphaModePolicy::off;
  return std::unexpected{"预设字段 alpha 只支持 force/auto/off。"};
}

std::expected<void, std::string> load_size_limit(const Json& object,
                                                  ImageSizeLimit& target) {
  if (!object.contains("size_limit")) return {};
  const auto& value = object.at("size_limit");
  if (auto valid = require_type(value, value.is_object(), "size_limit", "对象");
      !valid) return valid;
  if (value.contains("mode")) {
    if (auto valid = require_type(value.at("mode"), value.at("mode").is_string(),
                                  "size_limit.mode", "字符串"); !valid) return valid;
    const auto mode = value.at("mode").get<std::string>();
    if (mode == "auto") target.mode = ImageSizeLimitMode::automatic;
    else if (mode == "none") target.mode = ImageSizeLimitMode::none;
    else if (mode == "manual") target.mode = ImageSizeLimitMode::manual;
    else return std::unexpected{"预设字段 size_limit.mode 只支持 auto/none/manual。"};
  }
  const auto one = [&](std::string_view key, std::optional<int>& member) {
    return load_optional_int(value, key, 1, 1'000'000, member);
  };
  if (auto r = one("max_width", target.max_width); !r) return r;
  if (auto r = one("max_height", target.max_height); !r) return r;
  if (auto r = one("max_long_edge", target.max_long_edge); !r) return r;
  if (auto r = one("max_short_edge", target.max_short_edge); !r) return r;
  if (auto r = load_optional_int(value, "scale_percent", 1, 100, target.scale_percent); !r) return r;
  return {};
}

std::expected<void, std::string> load_format(const Json& object,
                                              PresetFormat& target) {
  if (!object.is_object()) {
    return std::unexpected{"预设 formats 中的格式值必须是对象。"};
  }
  if (auto r = load_int(object, "quality", 1, 100, target.quality); !r) return r;
  if (auto r = load_optional_int(object, "visual_quality", 1, 100,
                                 target.visual_quality); !r) return r;
  if (auto r = load_optional_int(object, "bit_depth", 1, 16, target.bit_depth);
      !r) return r;
  if (auto r = load_optional_int(object, "speed", 0, 10, target.speed); !r) return r;
  if (auto r = load_int(object, "jpegli_progressive_level", 0, 2,
                         target.jpegli_progressive_level); !r) return r;
  if (auto r = load_bool(object, "jpegli_optimize_huffman",
                          target.jpegli_optimize_huffman); !r) return r;
  if (auto r = load_bool(object, "jpegli_xyb", target.jpegli_xyb); !r) return r;
  if (auto r = load_bool(object, "jxl_jpeg_lossless", target.jxl_jpeg_lossless); !r) return r;
  if (auto r = load_int(object, "threads", 1, encoding_defaults::max_automatic_thread_budget,
                         target.max_jobs); !r) return r;
  if (auto r = load_u64(object, "memory_limit_bytes", 1ull << 50,
                         target.memory_limit_bytes); !r) return r;
  if (auto r = load_size_limit(object, target.image_size_limit); !r) return r;
  auto avif = parse_avif_encoder_value(object, "avif_encoder", target.avif_encoder);
  if (!avif) return std::unexpected{avif.error()};
  target.avif_encoder = *avif;
  auto representation = parse_avif_color_representation_value(
      object, "avif_color_representation", target.avif_color_representation);
  if (!representation) return std::unexpected{representation.error()};
  target.avif_color_representation = *representation;
  auto chroma = parse_chroma_value(object, "chroma", target.chroma_mode);
  if (!chroma) return std::unexpected{chroma.error()};
  target.chroma_mode = *chroma;
  auto alpha = parse_alpha_value(object, "alpha", target.alpha_policy);
  if (!alpha) return std::unexpected{alpha.error()};
  target.alpha_policy = *alpha;
  return {};
}

Json optional_int_json(const std::optional<int>& value) {
  return value ? Json(*value) : Json(nullptr);
}

Json size_limit_json(const ImageSizeLimit& limit) {
  std::string mode{"auto"};
  if (limit.mode == ImageSizeLimitMode::none) mode = "none";
  if (limit.mode == ImageSizeLimitMode::manual) mode = "manual";
  return Json{{"mode", mode},
              {"max_width", optional_int_json(limit.max_width)},
              {"max_height", optional_int_json(limit.max_height)},
              {"max_long_edge", optional_int_json(limit.max_long_edge)},
              {"max_short_edge", optional_int_json(limit.max_short_edge)},
              {"scale_percent", optional_int_json(limit.scale_percent)}};
}

Json format_json(const PresetFormat& value) {
  return Json{{"quality", value.quality},
              {"visual_quality", optional_int_json(value.visual_quality)},
              {"bit_depth", optional_int_json(value.bit_depth)},
              {"speed", optional_int_json(value.speed)},
              {"avif_encoder", avif_encoder_value(value.avif_encoder)},
              {"avif_color_representation",
               avif_color_representation_name(value.avif_color_representation)},
              {"chroma", chroma_mode_name(value.chroma_mode)},
              {"alpha", alpha_mode_policy_name(value.alpha_policy)},
              {"jpegli_progressive_level", value.jpegli_progressive_level},
              {"jpegli_optimize_huffman", value.jpegli_optimize_huffman},
              {"jpegli_xyb", value.jpegli_xyb},
              {"jxl_jpeg_lossless", value.jxl_jpeg_lossless},
              {"threads", value.max_jobs},
              {"memory_limit_bytes", value.memory_limit_bytes},
              {"size_limit", size_limit_json(value.image_size_limit)}};
}

std::expected<void, std::string> atomic_write(const fs::path& path,
                                               std::string_view text) {
  std::error_code ec;
  auto temporary = path;
  temporary += ".tmp";
  fs::remove(temporary, ec);
  {
    std::ofstream output{temporary, std::ios::binary | std::ios::trunc};
    if (!output) return std::unexpected{"无法创建预设临时文件。"};
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    output.flush();
    if (!output) {
      output.close();
      fs::remove(temporary, ec);
      return std::unexpected{"写入预设临时文件失败。"};
    }
  }
#ifdef _WIN32
  DWORD error = ERROR_SUCCESS;
  bool moved = false;
  for (int attempt = 0; attempt != 10; ++attempt) {
    if (MoveFileExW(temporary.c_str(), path.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE) {
      moved = true;
      break;
    }
    error = GetLastError();
    if (error != ERROR_ACCESS_DENIED && error != ERROR_SHARING_VIOLATION &&
        error != ERROR_LOCK_VIOLATION) {
      break;
    }
    Sleep(10);
  }
  if (!moved) {
    fs::remove(temporary, ec);
    return std::unexpected{std::format("原子替换预设文件失败，Windows 错误 {}。", error)};
  }
#else
  fs::rename(temporary, path, ec);
  if (ec) {
    const auto error = ec.message();
    fs::remove(temporary, ec);
    return std::unexpected{"原子替换预设文件失败：" + error};
  }
#endif
  return {};
}

std::expected<Json, std::string> read_document(const fs::path& path) {
  try {
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec || size > 2u * 1024u * 1024u) return std::unexpected{"文件不可读或超过 2 MiB 限制。"};
    std::ifstream input{path, std::ios::binary};
    if (!input) return std::unexpected{"无法读取预设文件。"};
    std::string raw(static_cast<std::size_t>(size), '\0');
    input.read(raw.data(), static_cast<std::streamsize>(raw.size()));
    if (!input || input.peek() != std::char_traits<char>::eof()) return std::unexpected{"读取期间预设文件发生变化。"};
    return Json::parse(strip_jsonc_comments(raw), nullptr, true, true);
  } catch (const std::exception& error) {
    return std::unexpected{std::format("预设 JSONC 无法解析：{}", error.what())};
  }
}

class PresetLock {
 public:
  explicit PresetLock(const fs::path& directory) {
#ifdef _WIN32
    handle_ = CreateFileW((directory / L".awj-lock").c_str(), GENERIC_READ | GENERIC_WRITE,
                         0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);
#else
    handle_ = ::open((directory / ".awj-lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (handle_ >= 0 && flock(handle_, LOCK_EX | LOCK_NB) != 0) { close(handle_); handle_ = -1; }
#endif
  }
  ~PresetLock() {
#ifdef _WIN32
    if (valid()) CloseHandle(handle_);
#else
    if (valid()) close(handle_);
#endif
  }
  bool valid() const noexcept {
#ifdef _WIN32
    return handle_ != INVALID_HANDLE_VALUE;
#else
    return handle_ >= 0;
#endif
  }
 private:
#ifdef _WIN32
  HANDLE handle_{INVALID_HANDLE_VALUE};
#else
  int handle_{-1};
#endif
};

std::string path_utf8(const fs::path& path) {
  const auto value = path.u8string();
  return {reinterpret_cast<const char*>(value.data()), value.size()};
}

fs::path path_from_utf8(std::string_view text) {
  return fs::path{std::u8string_view{reinterpret_cast<const char8_t*>(text.data()), text.size()}};
}

// The display name may change while its existing file stays at the same path.
// That makes an edit/rename one atomic replacement, with a single recovery record.
std::expected<void, std::string> restore_operation(const fs::path& directory, const Json& journal) {
  if (!journal.is_object() || journal.value("owner", "") != "AWJimage" ||
      journal.value("schema", 0) != 1 || !journal.contains("file") ||
      !journal["file"].is_string() || !journal.contains("before") ||
      !(journal["before"].is_null() || journal["before"].is_string())) {
    return std::unexpected{"预设恢复记录无效，未修改文件。"};
  }
  const auto file = path_from_utf8(journal["file"].get<std::string>());
  if (file != file.filename() || file.extension() != ".jsonc" || file.empty()) {
    return std::unexpected{"预设恢复记录包含非法路径。"};
  }
  if (journal["before"].is_string()) return atomic_write(directory / file, journal["before"].get<std::string>());
  std::error_code ec;
  fs::remove(directory / file, ec);
  if (ec) return std::unexpected{"无法回滚新建预设：" + ec.message()};
  return {};
}

std::expected<void, std::string> change_file(const fs::path& path,
    const std::optional<std::string>& content,
    const std::function<std::expected<void, std::string>()>& synchronize) {
  const auto journal_path = path.parent_path() / ".awj-operation.json";
  std::error_code ec;
  if (fs::exists(journal_path, ec) || ec) return std::unexpected{"存在未完成的预设操作，请重新打开程序恢复。"};
  Json before = nullptr;
  if (fs::exists(path, ec)) {
    const auto size = fs::file_size(path, ec);
    if (ec || size > 1024u * 1024u) return std::unexpected{"原预设不可读或超过 1 MiB。"};
    std::ifstream input{path, std::ios::binary};
    std::string raw(static_cast<std::size_t>(size), '\0');
    input.read(raw.data(), static_cast<std::streamsize>(raw.size()));
    if (!input) return std::unexpected{"无法备份原预设。"};
    before = std::move(raw);
  }
  if (ec) return std::unexpected{"无法检查原预设：" + ec.message()};
  const Json journal{{"owner", "AWJimage"}, {"schema", 1},
                     {"file", path_utf8(path.filename())}, {"before", before}};
  if (auto saved = atomic_write(journal_path, journal.dump()); !saved) return saved;
  std::expected<void, std::string> changed;
  try {
    if (content) changed = atomic_write(path, *content);
    else {
      fs::remove(path, ec);
      if (ec) changed = std::unexpected{"删除预设失败：" + ec.message()};
    }
    if (changed && synchronize) changed = synchronize();
  } catch (const std::exception& error) {
    changed = std::unexpected{error.what()};
  } catch (...) {
    changed = std::unexpected{"同步预设时发生未知错误。"};
  }
  if (!changed) {
    auto restored = restore_operation(path.parent_path(), journal);
    if (restored && synchronize) restored = synchronize();
    if (!restored) return std::unexpected{changed.error() + " 恢复尚未完成：" + restored.error()};
    fs::remove(journal_path, ec);
    return changed;
  }
  // Persist the completed state before dropping the journal, so interrupted
  // cleanup cannot undo an already synchronized menu on the next launch.
  auto committed = journal;
  committed["committed"] = true;
  if (auto result = atomic_write(journal_path, committed.dump()); !result) return result;
  fs::remove(journal_path, ec);
  if (ec) return std::unexpected{"预设已保存，但清理恢复记录失败：" + ec.message()};
  return {};
}

}  // namespace preset_detail

std::expected<std::filesystem::path, std::string> user_preset_directory() {
  auto directory = preset_detail::running_executable_directory();
  if (!directory) return std::unexpected{directory.error()};
  return *directory / "preset";
}

std::expected<void, std::string> recover_user_preset_change(
    const std::function<std::expected<void, std::string>()>& synchronize = {}) {
  auto directory = user_preset_directory();
  if (!directory) return std::unexpected{directory.error()};
  std::error_code ec;
  if (!std::filesystem::exists(*directory, ec)) return ec
      ? std::expected<void, std::string>{std::unexpected{ec.message()}}
      : std::expected<void, std::string>{};
  preset_detail::PresetLock lock{*directory};
  if (!lock.valid()) return std::unexpected{"另一进程正在修改预设。"};
  const auto path = *directory / ".awj-operation.json";
  if (!std::filesystem::exists(path, ec)) return ec
      ? std::expected<void, std::string>{std::unexpected{ec.message()}}
      : std::expected<void, std::string>{};
  auto journal = preset_detail::read_document(path);
  if (!journal) return std::unexpected{journal.error()};
  if (!journal->is_object() || journal->value("owner", "") != "AWJimage" ||
      journal->value("schema", 0) != 1) return std::unexpected{"预设恢复记录无效。"};
  if (!journal->value("committed", false)) {
    if (auto restored = preset_detail::restore_operation(*directory, *journal); !restored) return restored;
  }
  if (synchronize) {
    if (auto synced = synchronize(); !synced) return synced;
  }
  std::filesystem::remove(path, ec);
  if (ec) return std::unexpected{"无法清理预设恢复记录：" + ec.message()};
  return {};
}

std::string user_preset_format_key(OutputFormat format) {
  return std::string{preset_detail::format_keys[preset_detail::format_slot(format)]};
}

PresetFormat default_user_preset_format(OutputFormat format) {
  return preset_detail::default_format(format);
}

UserPreset default_user_preset() {
  UserPreset preset{};
  preset.formats = preset_detail::default_formats();
  return preset;
}

PresetFormat preset_format_from_config(const AppConfig& config) {
  PresetFormat preset = default_user_preset_format(config.output_format);
  if (config.output_format == OutputFormat::png) {
    preset.quality = config.quality;
    preset.visual_quality.reset();
  } else {
    preset.quality = config.quality;
    preset.visual_quality = config.visual_quality;
  }
  preset.bit_depth = config.bit_depth;
  preset.speed = config.speed;
  preset.avif_encoder = config.avif_encoder;
  preset.avif_color_representation = config.avif_color_representation;
  preset.chroma_mode = config.chroma_mode;
  preset.alpha_policy = config.alpha_policy;
  preset.jpegli_progressive_level = config.jpegli_progressive_level;
  preset.jpegli_optimize_huffman = config.jpegli_optimize_huffman;
  preset.jpegli_xyb = config.jpegli_xyb;
  preset.jxl_jpeg_lossless = config.jxl_jpeg_lossless;
  preset.max_jobs = config.max_jobs;
  preset.memory_limit_bytes = config.memory_limit_bytes;
  preset.image_size_limit = config.image_size_limit;
  return preset;
}

AppConfig config_from_user_preset(const UserPreset& preset,
                                  OutputFormat format) {
  AppConfig config = default_app_config();
  config.output_format = format;
  const auto& source = preset.formats[preset_detail::format_slot(format)];
  if (format == OutputFormat::png) {
    config.quality = source.quality;
    config.visual_quality.reset();
  } else {
    config.quality = source.quality;
    config.visual_quality = source.visual_quality;
  }
  config.bit_depth = source.bit_depth;
  config.speed = source.speed;
  config.avif_encoder = source.avif_encoder;
  config.avif_color_representation = source.avif_color_representation;
  config.chroma_mode = source.chroma_mode;
  config.alpha_policy = source.alpha_policy;
  config.jpegli_progressive_level = source.jpegli_progressive_level;
  config.jpegli_optimize_huffman = source.jpegli_optimize_huffman;
  config.jpegli_xyb = source.jpegli_xyb;
  config.jxl_jpeg_lossless = source.jxl_jpeg_lossless;
  config.max_jobs = source.max_jobs;
  config.memory_limit_bytes = source.memory_limit_bytes;
  config.image_size_limit = source.image_size_limit;
  return config;
}

std::expected<void, std::string> validate_user_preset(const UserPreset& preset) {
  if (preset.schema != user_preset_schema) {
    return std::unexpected{"预设 schema 只支持 1。"};
  }
  if (auto valid = preset_detail::validate_display_name(preset.name); !valid) {
    return valid;
  }
  if (preset.description.size() > 16 * 1024 ||
      std::ranges::any_of(preset.description, [](unsigned char ch) {
        return ch == 0;
      })) {
    return std::unexpected{"预设简介为空字符或长度超过 16 KiB。"};
  }
  if (auto filename = preset_detail::safe_file_stem(preset.name); !filename) {
    return std::unexpected{filename.error()};
  }
  for (std::size_t i = 0; i < preset.formats.size(); ++i) {
    auto config = config_from_user_preset(preset, preset_detail::format_from_slot(i));
    if (auto valid = validate_config(config); !valid) {
      return std::unexpected{std::format("预设 {} 参数无效：{}",
                                         preset_detail::format_keys[i], valid.error())};
    }
  }
  return {};
}

std::expected<UserPreset, std::string> load_user_preset_file(
    const std::filesystem::path& path) {
  try {
    std::error_code ec;
    if (std::filesystem::file_size(path, ec) > 1024u * 1024u || ec) {
      return std::unexpected{"预设文件超过 1 MiB 限制。"};
    }
    auto parsed = preset_detail::read_document(path);
    if (!parsed) return std::unexpected{parsed.error()};
    const auto& document = *parsed;
    if (!document.is_object()) return std::unexpected{"预设根节点必须是对象。"};
    if (!document.contains("schema") || !document.at("schema").is_number_integer() ||
        document.at("schema").get<int>() != user_preset_schema) {
      return std::unexpected{"预设 schema 必须为整数 1。"};
    }
    if (!document.contains("name") || !document.at("name").is_string()) {
      return std::unexpected{"预设 name 必须是字符串。"};
    }
    if (!document.contains("description") || !document.at("description").is_string()) {
      return std::unexpected{"预设 description 必须是字符串。"};
    }
    if (!document.contains("formats") || !document.at("formats").is_object()) {
      return std::unexpected{"预设 formats 必须是对象。"};
    }
    UserPreset preset = default_user_preset();
    preset.name = document.at("name").get<std::string>();
    preset.description = document.at("description").get<std::string>();
    preset.source_path = path;
    if (auto result = preset_detail::load_bool(document, "shell_menu", preset.shell_menu); !result) {
      return std::unexpected{result.error()};
    }
    const auto& formats = document.at("formats");
    for (std::size_t i = 0; i < preset.formats.size(); ++i) {
      const auto key = std::string{preset_detail::format_keys[i]};
      if (!formats.contains(key)) continue;
      if (auto result = preset_detail::load_format(formats.at(key), preset.formats[i]);
          !result) {
        return std::unexpected{std::format("预设 {}：{}", key, result.error())};
      }
    }
    if (auto valid = validate_user_preset(preset); !valid) {
      return std::unexpected{valid.error()};
    }
    return preset;
  } catch (const nlohmann::json::exception& error) {
    return std::unexpected{std::format("预设 JSONC 无法解析：{}", error.what())};
  } catch (const std::exception& error) {
    return std::unexpected{std::format("读取预设失败：{}", error.what())};
  }
}

std::expected<PresetCatalog, std::string> list_user_presets() {
  auto directory = user_preset_directory();
  if (!directory) return std::unexpected{directory.error()};
  PresetCatalog catalog{};
  std::error_code ec;
  if (!std::filesystem::exists(*directory, ec)) {
    if (ec) return std::unexpected{"无法读取预设目录：" + ec.message()};
    return catalog;
  }
  std::map<std::string, std::size_t> name_counts;
  for (const auto& entry : std::filesystem::directory_iterator(*directory, ec)) {
    if (ec) return std::unexpected{"无法枚举预设目录：" + ec.message()};
    if (!entry.is_regular_file(ec) || ec || entry.path().extension() != ".jsonc") continue;
    auto document = preset_detail::read_document(entry.path());
    if (document && document->is_object() && document->contains("name") && (*document)["name"].is_string()) {
      ++name_counts[preset_detail::upper_ascii((*document)["name"].get<std::string>())];
    }
    auto loaded = load_user_preset_file(entry.path());
    if (!loaded) {
      catalog.errors.push_back(std::format("{}：{}", preset_detail::path_utf8(entry.path().filename()),
                                           loaded.error()));
      continue;
    }
    catalog.presets.push_back(std::move(*loaded));
  }
  std::erase_if(catalog.presets, [&](const UserPreset& preset) {
    if (name_counts[preset_detail::upper_ascii(preset.name)] <= 1) return false;
    catalog.errors.push_back(std::format("重复预设名称“{}”：{}，已排除。", preset.name,
        preset_detail::path_utf8(preset.source_path.filename())));
    return true;
  });
  std::ranges::sort(catalog.presets, {}, &UserPreset::name);
  return catalog;
}

std::expected<UserPreset, std::string> find_user_preset(std::string_view name) {
  auto catalog = list_user_presets();
  if (!catalog) return std::unexpected{catalog.error()};
  const auto key = preset_detail::upper_ascii(std::string{name});
  const auto it = std::ranges::find_if(catalog->presets, [&](const auto& preset) {
    return preset_detail::upper_ascii(preset.name) == key;
  });
  if (it == catalog->presets.end()) {
    std::string available;
    for (const auto& preset : catalog->presets) {
      if (!available.empty()) available += "、";
      available += preset.name;
    }
    return std::unexpected{available.empty()
                               ? std::format("未找到名称为“{}”的用户预设；预设目录中没有有效预设。",
                                             name)
                               : std::format("未找到名称为“{}”的用户预设；可用预设：{}。",
                                             name, available)};
  }
  return *it;
}

std::expected<ParseResult, std::string> parse_arguments_with_user_preset(
    const std::vector<std::wstring>& args) {
  auto parsed = parse_arguments(args);
  if (!parsed || parsed->should_exit ||
      (!parsed->preset_name && !parsed->preset_file)) {
    return parsed;
  }
  if (parsed->preset_name && parsed->preset_file) {
    return std::unexpected{"--preset 与 --preset-file 不能同时使用。"};
  }
  auto preset = parsed->preset_file
                    ? load_user_preset_file(*parsed->preset_file)
                    : find_user_preset(
                          preset_detail::utf8_from_wide_for_preset(*parsed->preset_name));
  if (!preset) return std::unexpected{preset.error()};
  auto base = config_from_user_preset(*preset, parsed->config.output_format);
  auto resolved = parse_arguments_with_preset_base(args, std::move(base));
  if (!resolved) return std::unexpected{resolved.error()};
  return resolved;
}

std::expected<std::filesystem::path, std::string> save_user_preset(
    const UserPreset& preset, bool overwrite,
    const std::function<std::expected<void, std::string>()>& synchronize = {}) {
  if (auto valid = validate_user_preset(preset); !valid) {
    return std::unexpected{valid.error()};
  }
  auto directory = user_preset_directory();
  if (!directory) return std::unexpected{directory.error()};
  auto stem = preset_detail::safe_file_stem(preset.name);
  if (!stem) return std::unexpected{stem.error()};
  const auto desired_path = *directory / preset_detail::path_from_utf8(*stem + ".jsonc");
  auto path = desired_path;
  std::error_code ec;
  std::filesystem::create_directories(*directory, ec);
  if (ec) return std::unexpected{"无法创建预设目录：" + ec.message()};
  preset_detail::PresetLock lock{*directory};
  if (!lock.valid()) return std::unexpected{"另一进程正在修改预设。"};
  if (overwrite) {
    path = std::filesystem::canonical(preset.source_path, ec);
    if (ec || path.parent_path() != std::filesystem::canonical(*directory, ec) || ec ||
        path.extension() != ".jsonc" || std::filesystem::is_symlink(preset.source_path, ec) || ec) {
      return std::unexpected{"只能编辑预设目录中已选择的原文件。"};
    }
  }
  if (std::filesystem::exists(desired_path, ec) &&
      (!overwrite || !std::filesystem::equivalent(path, desired_path, ec))) {
    return std::unexpected{"预设名称或生成的文件名已被占用。"};
  }
  if (ec) return std::unexpected{"无法检查预设文件：" + ec.message()};
  std::size_t injected_count = preset.shell_menu ? 1 : 0;
  for (const auto& entry : std::filesystem::directory_iterator(*directory, ec)) {
    if (ec) return std::unexpected{"无法枚举预设目录：" + ec.message()};
    if (!entry.is_regular_file(ec) || ec || entry.path().extension() != ".jsonc") continue;
    if (overwrite && std::filesystem::equivalent(entry.path(), path, ec)) continue;
    if (ec) return std::unexpected{"无法检查原预设文件：" + ec.message()};
    auto document = preset_detail::read_document(entry.path());
    if (!document || !document->is_object()) continue;
    if (document->contains("name") && (*document)["name"].is_string() &&
        preset_detail::upper_ascii((*document)["name"].get<std::string>()) == preset_detail::upper_ascii(preset.name)) {
      return std::unexpected{"预设名称已存在；创建和改名不能覆盖其他预设。"};
    }
    if (document->contains("shell_menu") && (*document)["shell_menu"].is_boolean() &&
        (*document)["shell_menu"].get<bool>()) ++injected_count;
  }
  if (injected_count > 10) return std::unexpected{"最多同时注入 10 个预设，请先取消其他预设的注入。"};
  preset_detail::Json formats = preset_detail::Json::object();
  for (std::size_t i = 0; i < preset.formats.size(); ++i) {
    formats[std::string{preset_detail::format_keys[i]}] =
        preset_detail::format_json(preset.formats[i]);
  }
  const preset_detail::Json document{{"schema", user_preset_schema},
                                     {"name", preset.name},
                                     {"description", preset.description},
                                     {"shell_menu", preset.shell_menu},
                                     {"formats", std::move(formats)}};
  const auto content = document.dump(2) + "\n";
  if (auto saved = preset_detail::change_file(path, content, synchronize); !saved) {
    return std::unexpected{saved.error()};
  }
  return path;
}

std::expected<void, std::string> delete_user_preset(
    const UserPreset& preset,
    const std::function<std::expected<void, std::string>()>& synchronize = {}) {
  auto directory = user_preset_directory();
  if (!directory) return std::unexpected{directory.error()};
  preset_detail::PresetLock lock{*directory};
  if (!lock.valid()) return std::unexpected{"另一进程正在修改预设。"};
  std::error_code ec;
  const auto path = std::filesystem::canonical(preset.source_path, ec);
  if (ec || path.parent_path() != std::filesystem::canonical(*directory, ec) || ec ||
      path.extension() != ".jsonc" || std::filesystem::is_symlink(preset.source_path, ec) || ec) {
    return std::unexpected{"只能删除预设目录中已选择的用户预设；内置默认不可删除。"};
  }
  return preset_detail::change_file(path, std::nullopt, synchronize);
}

std::expected<std::vector<std::wstring>, std::string> injected_user_preset_names() {
  auto catalog = list_user_presets();
  if (!catalog) return std::unexpected{catalog.error()};
  std::vector<std::wstring> names;
  for (const auto& preset : catalog->presets) {
    if (preset.shell_menu) names.push_back(preset_detail::path_from_utf8(preset.name).wstring());
  }
  if (names.size() > 10) return std::unexpected{"已注入预设超过 10 个，请修正预设配置。"};
  return names;
}

}  // namespace awj
