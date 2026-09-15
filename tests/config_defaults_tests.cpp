#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

import awj.config;
import awj.codec;
import awj.encoding_defaults;
import awj.preset;

namespace {

int fail(const char* message) {
  std::cerr << message << '\n';
  return 1;
}

}  // namespace

int main() {
  if (awj::automatic_thread_budget(0) != 1 ||
      awj::automatic_thread_budget(1) != 1 ||
      awj::automatic_thread_budget(2) != 1 ||
      awj::automatic_thread_budget(4) != 3 ||
      awj::automatic_thread_budget(5) != 3 ||
      awj::automatic_thread_budget(11) != 9 ||
      awj::automatic_thread_budget(12) != 8 ||
      awj::automatic_thread_budget(32) != 28 ||
      awj::automatic_thread_budget(256) != 128) {
    return fail("automatic thread budget reservation rules changed.");
  }

  const auto defaults = awj::default_app_config();
  if (defaults.input_path.generic_string() !=
      awj::encoding_defaults::default_input_path_text) {
    return fail("default input path does not come from encoding defaults.");
  }
  if (defaults.output_template !=
      awj::encoding_defaults::default_output_template) {
    return fail(
        "default output template does not come from encoding defaults.");
  }
  if (defaults.quality != awj::encoding_defaults::default_avif_quality) {
    return fail("default AVIF quality mismatch.");
  }
  if (defaults.avif_color_representation !=
          awj::AvifColorRepresentation::yuv ||
      defaults.append_png_suffix) {
    return fail("default AVIF color representation or filename suffix mismatch.");
  }
  if (awj::encoding_defaults::default_avif_quality != 70 ||
      awj::encoding_defaults::default_jxl_quality != 85) {
    return fail("AVIF/JXL default quality literals mismatch.");
  }
#ifdef _WIN32
  if (!defaults.allow_wic_fallback) {
    return fail("Windows should enable WIC fallback by default.");
  }
#else
  if (defaults.allow_wic_fallback) {
    return fail("Non-Windows builds should disable WIC fallback by default.");
  }
#endif

  if (!defaults.visual_quality_fallback) {
    return fail("visual-quality fallback should be enabled by default.");
  }
  if (defaults.memory_limit_bytes !=
      awj::encoding_defaults::default_memory_limit_bytes) {
    return fail("default memory limit mismatch.");
  }
  if (defaults.encode_timeout_minutes !=
      awj::encoding_defaults::default_encode_timeout_minutes) {
    return fail("default encode timeout mismatch.");
  }

  if (awj::default_quality_for(awj::OutputFormat::avif) !=
          awj::encoding_defaults::default_avif_quality ||
      awj::default_quality_for(awj::OutputFormat::webp) !=
          awj::encoding_defaults::default_webp_quality ||
      awj::default_quality_for(awj::OutputFormat::jxl) !=
          awj::encoding_defaults::default_jxl_quality ||
      awj::default_quality_for(awj::OutputFormat::jpgli) !=
          awj::encoding_defaults::default_jpegli_quality) {
    return fail("format quality defaults mismatch.");
  }

  if (awj::default_speed_for(awj::OutputFormat::webp) !=
          awj::encoding_defaults::default_webp_native_speed ||
      awj::default_speed_for(awj::OutputFormat::avif) !=
          awj::encoding_defaults::default_avif_native_speed ||
      awj::default_speed_for(awj::OutputFormat::jxl) !=
          awj::encoding_defaults::default_jxl_native_speed ||
      awj::default_speed_for(awj::OutputFormat::jpgli) !=
          awj::encoding_defaults::default_jpegli_native_speed) {
    return fail("format speed defaults mismatch.");
  }
  if (awj::encoding_defaults::default_avif_native_speed != 5 ||
      awj::encoding_defaults::default_webp_native_speed != 4 ||
      awj::encoding_defaults::default_jxl_native_speed != 6) {
    return fail("format speed default literals mismatch.");
  }

  auto quoted = awj::parse_arguments(
      {L"--input", L"  \"D:\\example.jxr\"  ", L"--output",
       L"\"D:\\out dir\""});
  if (!quoted || quoted->config.input_path != std::filesystem::path{L"D:\\example.jxr"} ||
      quoted->config.output_dir != std::filesystem::path{L"D:\\out dir"}) {
    return fail("quoted CLI paths were not normalized.");
  }
  quoted = awj::parse_arguments({L"--input", L"\"D:\\example.jxr"});
  if (quoted || quoted.error().find("双引号") == std::string::npos) {
    return fail("unpaired quoted CLI input was not rejected.");
  }
  if (awj::map_jxl_speed_to_effort(0).codec_value != 10 ||
      awj::map_jxl_speed_to_effort(6).codec_value != 4 ||
      awj::map_jxl_speed_to_effort(10).codec_value != 1) {
    return fail("JXL speed to effort mapping mismatch.");
  }

  auto webp_cfg = awj::default_app_config();
  awj::apply_format_defaults(webp_cfg, awj::OutputFormat::webp);
  if (webp_cfg.output_format != awj::OutputFormat::webp ||
      webp_cfg.quality != awj::encoding_defaults::default_webp_quality) {
    return fail("format defaults did not apply WebP quality.");
  }

  auto parsed = awj::parse_arguments({L"--format", L"jxl"});
  if (!parsed) {
    std::cerr << parsed.error() << '\n';
    return 1;
  }
  if (parsed->config.quality != awj::encoding_defaults::default_jxl_quality) {
    return fail("CLI JXL default quality mismatch.");
  }

  parsed = awj::parse_arguments({L"--format", L"jpgli"});
  if (!parsed || parsed->config.output_format != awj::OutputFormat::jpgli ||
      parsed->config.quality != awj::encoding_defaults::default_jpegli_quality) {
    return fail("CLI JPGLI format alias did not parse.");
  }

  parsed = awj::parse_arguments({L"--format", L"jpegli"});
  if (!parsed || parsed->config.output_format != awj::OutputFormat::jpgli) {
    return fail("CLI Jpegli format alias did not parse.");
  }

  parsed = awj::parse_arguments(
      {L"--format", L"jpgli", L"--chroma", L"422",
       L"--jpegli-progressive-level", L"0", L"--jpegli-fixed-huffman",
       L"--jpegli-xyb"});
  if (!parsed || parsed->config.output_format != awj::OutputFormat::jpgli ||
      parsed->config.chroma_mode != awj::ChromaMode::yuv422 ||
      parsed->config.jpegli_progressive_level != 0 ||
      parsed->config.jpegli_optimize_huffman ||
      !parsed->config.jpegli_xyb) {
    return fail("CLI JPGLI advanced options did not parse.");
  }

  parsed = awj::parse_arguments(
      {L"--format", L"jpgli", L"--jpegli-progressive-level", L"2",
       L"--jpegli-fixed-huffman"});
  if (parsed ||
      parsed.error().find("渐进 JPEG 需要优化哈夫曼表") == std::string::npos) {
    return fail("CLI JPGLI invalid progressive/fixed huffman combination was not rejected.");
  }

  parsed = awj::parse_arguments({L"--format", L"jpg"});
  if (parsed || parsed.error().find("输出格式不支持") == std::string::npos) {
    return fail("CLI should not accept plain jpg as the JPGLI entry.");
  }

  parsed = awj::parse_arguments({L"--format", L"jpgli", L"--alpha", L"force"});
  if (parsed ||
      parsed.error().find("JPGLI 不支持 alpha 输出") == std::string::npos) {
    return fail("CLI did not reject forced alpha for JPGLI.");
  }

  parsed = awj::parse_arguments({L"--format", L"webp", L"--quality", L"82"});
  if (!parsed) {
    std::cerr << parsed.error() << '\n';
    return 1;
  }
  if (parsed->config.quality != 82) {
    return fail("explicit CLI quality was overwritten by defaults.");
  }

  parsed = awj::parse_arguments({L"--format", L"jpgli", L"--speed", L"6"});
  if (parsed || parsed.error().find("JPGLI 不支持 --speed") == std::string::npos) {
    return fail("CLI should reject speed for JPGLI.");
  }
  parsed = awj::parse_arguments({L"--format", L"png", L"--speed", L"6"});
  if (parsed || parsed.error().find("PNG 不支持 --speed") == std::string::npos) {
    return fail("CLI should reject speed for PNG.");
  }

  parsed = awj::parse_arguments({L"--shell-convert", L"--format", L"png",
                                 L"C:\\img\\a.webp", L"C:\\img\\b.webp",
                                 L"C:\\img\\a.webp"});
  if (!parsed || parsed->config.output_policy != awj::OutputPolicy::shell ||
      parsed->shell_inputs.size() != 2 ||
      parsed->shell_inputs[0] != std::filesystem::path{L"C:\\img\\a.webp"} ||
      parsed->shell_inputs[1] != std::filesystem::path{L"C:\\img\\b.webp"}) {
    return fail("CLI shell conversion inputs were not parsed and de-duplicated.");
  }

  parsed = awj::parse_arguments({L"--collision", L"number"});
  if (!parsed || parsed->config.collision_mode != awj::CollisionMode::suffix_number) {
    return fail("CLI numbered collision mode did not parse.");
  }

  auto size_cfg = awj::default_app_config();
  size_cfg.image_size_limit.mode = awj::ImageSizeLimitMode::manual;
  if (awj::limited_dimensions(4000, 3000, size_cfg)) {
    return fail("blank manual scale should behave as 100 percent.");
  }
  size_cfg.image_size_limit.scale_percent = 80;
  if (awj::limited_dimensions(4000, 3000, size_cfg) !=
      std::optional{std::pair<std::size_t, std::size_t>{3200, 2400}}) {
    return fail("80 percent manual scale produced wrong dimensions.");
  }
  size_cfg.image_size_limit.scale_percent = 100;
  if (awj::limited_dimensions(4000, 3000, size_cfg)) {
    return fail("100 percent manual scale should not resize.");
  }
  size_cfg.image_size_limit.scale_percent = 1;
  if (awj::limited_dimensions(2, 1, size_cfg) !=
      std::optional{std::pair<std::size_t, std::size_t>{1, 1}}) {
    return fail("manual scale did not clamp dimensions to at least one pixel.");
  }
  size_cfg.image_size_limit.scale_percent = 80;
  if (awj::limited_dimensions(5, 3, size_cfg) !=
      std::optional{std::pair<std::size_t, std::size_t>{4, 2}}) {
    return fail("manual scale did not floor odd dimensions.");
  }
  size_cfg.image_size_limit.max_width = 3500;
  size_cfg.image_size_limit.max_height = 2100;
  size_cfg.image_size_limit.max_long_edge = 2800;
  size_cfg.image_size_limit.max_short_edge = 1800;
  if (awj::limited_dimensions(4000, 3000, size_cfg) !=
      std::optional{std::pair<std::size_t, std::size_t>{2400, 1800}}) {
    return fail("manual percentage and edge limits did not use the smallest scale.");
  }
  size_cfg.image_size_limit.mode = awj::ImageSizeLimitMode::automatic;
  if (awj::limited_dimensions(4000, 3000, size_cfg)) {
    return fail("scale percent unexpectedly affected automatic size mode.");
  }

  parsed = awj::parse_arguments({L"--image-size-limit", L"manual", L"--scale-percent", L"80"});
  if (!parsed || parsed->config.image_size_limit.scale_percent.value_or(0) != 80) {
    return fail("CLI scale percent did not parse.");
  }
  for (const auto* invalid_scale : {L"0", L"-1", L"101", L"abc"}) {
    parsed = awj::parse_arguments({L"--scale-percent", invalid_scale});
    if (parsed) return fail("invalid CLI scale percent was accepted.");
  }

  parsed = awj::parse_arguments({L"--image-size-limit", L"manual", L"--max-width", L"1600",
                                 L"--max-height", L"1200", L"--max-long-edge", L"2000",
                                 L"--max-short-edge", L"900", L"--scale-percent", L"80"});
  if (!parsed || parsed->config.image_size_limit.mode != awj::ImageSizeLimitMode::manual ||
      parsed->config.image_size_limit.max_width.value_or(0) != 1600 ||
      parsed->config.image_size_limit.max_height.value_or(0) != 1200 ||
      parsed->config.image_size_limit.max_long_edge.value_or(0) != 2000 ||
      parsed->config.image_size_limit.max_short_edge.value_or(0) != 900 ||
      parsed->config.image_size_limit.scale_percent.value_or(0) != 80) {
    return fail("CLI image size limit did not parse.");
  }

  parsed = awj::parse_arguments({L"--suffix-number"});
  if (!parsed || parsed->config.collision_mode != awj::CollisionMode::suffix_number) {
    return fail("CLI --suffix-number did not parse.");
  }

  parsed = awj::parse_arguments({L"C:\\img\\a.webp"});
  if (parsed || parsed.error().find("未知参数") == std::string::npos) {
    return fail("normal CLI mode should still reject positional inputs.");
  }

  parsed = awj::parse_arguments({L"--preset", L"fast"});
  if (!parsed || !parsed->preset_name || *parsed->preset_name != L"fast" ||
      parsed->config.quality != awj::encoding_defaults::default_avif_quality) {
    return fail("user preset selector did not preserve the built-in defaults.");
  }

  const auto preset_test_path = std::filesystem::temp_directory_path() /
                                "awjimage-config-defaults-preset.jsonc";
  std::error_code preset_test_ec;
  std::filesystem::remove(preset_test_path, preset_test_ec);
  {
    std::ofstream output{preset_test_path, std::ios::binary | std::ios::trunc};
  output << R"jsonc(// JSONC comments are accepted.
{
  "schema": 1,
  "name": "测试预设",
  "description": "only WebP overrides the hard-coded defaults",
  "formats": {
    "avif": { "avif_color_representation": "rgb" },
    "webp": { "quality": 61, "speed": 3 }
  }
}
)jsonc";
  }
  const auto loaded_preset = awj::load_user_preset_file(preset_test_path);
  if (!loaded_preset || loaded_preset->formats[1].quality != 61 ||
      loaded_preset->formats[1].speed.value_or(-1) != 3 ||
      loaded_preset->formats[0].avif_color_representation !=
          awj::AvifColorRepresentation::rgb_identity ||
      loaded_preset->formats[0].quality !=
          awj::encoding_defaults::default_avif_quality) {
    std::filesystem::remove(preset_test_path, preset_test_ec);
    return fail("user preset did not validate JSONC or fill missing formats from defaults.");
  }
  {
    std::ofstream output{preset_test_path, std::ios::binary | std::ios::trunc};
    output << R"json({
  "schema": 1,
  "name": "legacy optional scalar",
  "description": "1.0.4 emitted one-item arrays for optional numbers",
  "formats": { "avif": { "visual_quality": [null], "speed": [5] } }
})json";
  }
  const auto legacy_preset = awj::load_user_preset_file(preset_test_path);
  if (!legacy_preset || legacy_preset->formats[0].visual_quality ||
      legacy_preset->formats[0].speed.value_or(-1) != 5 ||
      legacy_preset->formats[0].avif_color_representation !=
          awj::AvifColorRepresentation::yuv) {
    std::filesystem::remove(preset_test_path, preset_test_ec);
    return fail("legacy one-item optional preset arrays were not migrated on load.");
  }
  {
    std::ofstream output{preset_test_path, std::ios::binary | std::ios::trunc};
    output << R"jsonc(// JSONC comments are accepted.
{
  "schema": 1,
  "name": "测试预设",
  "description": "only WebP overrides the hard-coded defaults",
  "formats": {
    "webp": { "quality": 61, "speed": 3 }
  }
}
)jsonc";
  }
  const auto preset_path_wide = preset_test_path.wstring();
  auto preset_args = awj::parse_arguments_with_user_preset(
      {L"--quality", L"82", L"--preset-file", preset_path_wide,
       L"--format", L"webp"});
  if (!preset_args || preset_args->config.quality != 82 ||
      preset_args->config.speed.value_or(-1) != 3) {
    std::filesystem::remove(preset_test_path, preset_test_ec);
    return fail("explicit CLI parameters did not override a user preset.");
  }
  preset_args = awj::parse_arguments_with_user_preset(
      {L"--format", L"webp", L"--preset-file", preset_path_wide});
  if (!preset_args || preset_args->config.quality != 61 ||
      preset_args->config.speed.value_or(-1) != 3) {
    std::filesystem::remove(preset_test_path, preset_test_ec);
    return fail("preset selection depended on CLI argument order.");
  }
  awj::UserPreset png_preset = awj::default_user_preset();
  png_preset.formats.back().quality = 37;
  png_preset.formats.back().visual_quality = 42;
  const auto png_preset_config =
      awj::config_from_user_preset(png_preset, awj::OutputFormat::png);
  if (png_preset_config.quality != 37 || png_preset_config.visual_quality) {
    std::filesystem::remove(preset_test_path, preset_test_ec);
    return fail("PNG user preset discarded ordinary quality or retained visual search.");
  }
  awj::AppConfig png_config = awj::default_app_config();
  png_config.output_format = awj::OutputFormat::png;
  png_config.quality = 23;
  png_config.visual_quality = 55;
  const auto png_preset_format = awj::preset_format_from_config(png_config);
  if (png_preset_format.quality != 23 || png_preset_format.visual_quality) {
    std::filesystem::remove(preset_test_path, preset_test_ec);
    return fail("PNG preset serialization discarded ordinary quality or retained visual search.");
  }
  awj::UserPreset reserved_name = awj::default_user_preset();
  reserved_name.name = "CON";
  if (awj::validate_user_preset(reserved_name)) {
    std::filesystem::remove(preset_test_path, preset_test_ec);
    return fail("Windows-reserved preset filename was accepted.");
  }
  {
    std::ofstream output{preset_test_path, std::ios::binary | std::ios::trunc};
    output << R"json({"schema":2,"name":"bad","description":"","formats":{}})json";
  }
  if (awj::load_user_preset_file(preset_test_path)) {
    std::filesystem::remove(preset_test_path, preset_test_ec);
    return fail("unsupported user preset schema was accepted.");
  }
  std::filesystem::remove(preset_test_path, preset_test_ec);

#ifdef _WIN32
  parsed = awj::parse_arguments({L"--preserve-creation-time",
                                 L"--preserve-modification-time",
                                 L"--preserve-access-time"});
  if (!parsed || !parsed->config.preserve_creation_time ||
      !parsed->config.preserve_modification_time ||
      !parsed->config.preserve_access_time) {
    return fail("Windows timestamp preservation flags did not parse.");
  }
#else
  parsed = awj::parse_arguments({L"--preserve-creation-time"});
  if (parsed || parsed.error().find("未知参数") == std::string::npos) {
    return fail("Linux CLI must reject Windows timestamp preservation flags.");
  }
#endif

  parsed =
      awj::parse_arguments({L"--avif-encoder", L"aom", L"--chroma", L"444"});
  if (!parsed || parsed->config.avif_encoder != awj::AvifEncoderMode::aom ||
      parsed->config.chroma_mode != awj::ChromaMode::yuv444) {
    return fail("CLI AVIF encoder selection did not parse.");
  }

  parsed = awj::parse_arguments(
      {L"--avif-color-representation", L"source", L"--append-png-suffix"});
  if (!parsed || parsed->config.avif_color_representation !=
                      awj::AvifColorRepresentation::source ||
      !parsed->config.append_png_suffix) {
    return fail("CLI AVIF color representation or AVIF.png suffix did not parse.");
  }
  parsed = awj::parse_arguments(
      {L"--avif-color-representation", L"rgb", L"--chroma", L"420"});
  if (parsed || parsed.error().find("必须使用 4:4:4") == std::string::npos) {
    return fail("CLI did not reject RGB Identity with 420 chroma.");
  }
  parsed = awj::parse_arguments(
      {L"--avif-color-representation", L"rgb", L"--avif-encoder", L"svt"});
  if (parsed || parsed.error().find("已移除") == std::string::npos) {
    return fail("CLI did not reject RGB Identity with SVT.");
  }
  parsed = awj::parse_arguments({L"--matrix-coefficients", L"0"});
  if (parsed || parsed.error().find("YUV 颜色表示") == std::string::npos) {
    return fail("default YUV unexpectedly accepted an Identity matrix.");
  }
  parsed = awj::parse_arguments(
      {L"--format", L"webp", L"--append-png-suffix"});
  if (parsed || parsed.error().find("仅可与 AVIF") == std::string::npos) {
    return fail("non-AVIF output unexpectedly accepted the AVIF.png suffix.");
  }

  for (const auto* removed : {L"svt", L"svt-av1", L"svt-av1-hdr", L"zenrav1e"}) {
    parsed = awj::parse_arguments({L"--avif-encoder", removed});
    if (parsed || parsed.error().find("已移除") == std::string::npos)
      return fail("Removed encoder was accepted or silently substituted.");
  }
  for (const auto* removed : {L"--svtav1hdr-crf", L"--svtav1hdr-preset",
       L"--svtav1hdr-tune", L"--svtav1hdr-keyint", L"--svtav1hdr-params",
       L"--mastering-display", L"--content-light", L"--experimental-encoders",
       L"--large-image-priority"}) {
    parsed = awj::parse_arguments({removed, L"1"});
    if (parsed || parsed.error().find("已移除") == std::string::npos)
      return fail("Removed encoder-specific option was accepted.");
  }
  parsed = awj::parse_arguments({L"--avif-encoder", L"aom", L"--color-primaries", L"9",
      L"--transfer-characteristics", L"16", L"--matrix-coefficients", L"9", L"--color-range", L"0"});
  if (!parsed || parsed->config.color_primaries != 9 || parsed->config.transfer_characteristics != 16 ||
      parsed->config.matrix_coefficients != 9 || parsed->config.color_range != 0)
    return fail("AOM explicit CICP configuration was lost.");

  parsed = awj::parse_arguments({L"--avif-encoder", L"rav1e"});
  if (parsed ||
      parsed.error().find("AVIF encoder 不支持") == std::string::npos) {
    return fail("CLI did not reject rav1e encoder.");
  }

  parsed = awj::parse_arguments({L"--no-wic-fallback"});
  if (!parsed || parsed->config.allow_wic_fallback) {
    return fail("CLI did not disable WIC fallback.");
  }

  parsed = awj::parse_arguments(
      {L"--backend", L"magick", L"--magick-path", L"tools"});
  if (parsed || parsed.error().find("未知参数") == std::string::npos) {
    return fail("CLI still accepts removed external Magick adapter options.");
  }

  parsed = awj::parse_arguments({L"--max-resolution", L"1600"});
  if (parsed || parsed.error().find("未知参数") == std::string::npos) {
    return fail("CLI still accepts removed max-resolution option.");
  }

  const auto help = awj::help_text();
  if (help.find(std::format("AVIF q{}",
                            awj::encoding_defaults::default_avif_quality)) ==
          std::string::npos ||
      help.find(std::format("WebP q{}",
                            awj::encoding_defaults::default_webp_quality)) ==
          std::string::npos ||
      help.find(std::format("JPGLI q{}",
                            awj::encoding_defaults::default_jpegli_quality)) ==
          std::string::npos ||
      help.find("jpgli|jpegli") == std::string::npos ||
      help.find("--jpegli-progressive-level") == std::string::npos ||
      help.find("--avif-encoder") == std::string::npos ||
      help.find("--experimental-encoders") != std::string::npos ||
      help.find("zenrav1e") != std::string::npos ||
      help.find("--svtav1hdr-crf") != std::string::npos ||
      help.find("--svtav1hdr-keyint") != std::string::npos ||
      help.find("--max-resolution") != std::string::npos) {
    return fail("help text does not reflect encoding defaults.");
  }

  // 帮助文本必须描述实际实现。默认颜色表示是 YUV，只有显式 source/rgb 设置
  // 才允许 Identity；HDR 源的 CICP 也不会被 BT.709 覆盖。
  if (help.find("按 auto 色度规则重编码") == std::string::npos ||
      help.find("默认 YUV") == std::string::npos ||
      help.find("--avif-color-representation <yuv|source|rgb>") ==
          std::string::npos ||
      help.find("--append-png-suffix") == std::string::npos ||
      help.find("按默认 420 重编码") != std::string::npos) {
    return fail("help text does not describe the AVIF color representation contract.");
  }

  if (help.find("含 PQ/HLG 等 HDR 传输函数") == std::string::npos ||
      help.find("源与用户都没有时才回退 BT.709") == std::string::npos) {
    return fail("help text does not describe HDR-preserving CICP precedence.");
  }

#ifdef _WIN32
  if (help.find("--preserve-creation-time") == std::string::npos ||
      help.find("--preserve-modification-time") == std::string::npos ||
      help.find("--preserve-access-time") == std::string::npos) {
    return fail("Windows timestamp preservation flags are missing from help.");
  }
#else
  if (help.find("--preserve-creation-time") != std::string::npos ||
      help.find("--preserve-modification-time") != std::string::npos ||
      help.find("--preserve-access-time") != std::string::npos) {
    return fail("Linux help lists Windows-only timestamp flags.");
  }
#endif

  static_assert(awj::encoding_defaults::default_experimental_clamped_grid_padding,
                "clamped grid padding default changed; update the help text.");
  if (help.find("较小的右列和底行 cell") == std::string::npos ||
      help.find("--chroma 444") == std::string::npos ||
      help.find("当前编码仍会拒绝") != std::string::npos) {
    return fail("help text does not describe clamped grid edge cells.");
  }

  return 0;
}
