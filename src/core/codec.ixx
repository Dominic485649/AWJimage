module;

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <limits>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

export module awj.codec;

import awj.config;
import awj.encoding_defaults;
import awj.image;
import awj.large_image_plan;
import awj.resource_planner;
import awj.visual_quality;

export namespace awj {

namespace fs = std::filesystem;

enum class BackendId {
  native,
};

enum class CodecFeature : std::uint32_t {
  none = 0,
  lossless = 1u << 0,
  alpha = 1u << 1,
  thread_control = 1u << 2,
  visual_quality_search = 1u << 3,
};

constexpr CodecFeature operator|(CodecFeature left, CodecFeature right) noexcept {
  return static_cast<CodecFeature>(static_cast<std::uint32_t>(left) |
                                   static_cast<std::uint32_t>(right));
}

constexpr bool has_feature(CodecFeature features, CodecFeature feature) noexcept {
  return (static_cast<std::uint32_t>(features) &
          static_cast<std::uint32_t>(feature)) != 0;
}

struct CodecCapabilities {
  OutputFormat output_format{};
  CodecFeature features{CodecFeature::none};
  int min_quality{1};
  int max_quality{100};
  int min_speed{0};
  int max_speed{10};
  std::vector<int> bit_depths{};
};

struct SpeedMapping {
  int user_speed{};
  int codec_value{};
  std::string codec_key{};
};

struct AvifEncoderCapability {
  AvifEncoderMode mode{AvifEncoderMode::automatic};
  std::string id{};
  std::vector<ChromaMode> chroma_modes{};
  std::vector<int> bit_depths{};
  bool supports_alpha{};
  bool supports_avif_grid{};
  std::optional<std::uint32_t> max_single_image_width{};
  std::optional<std::uint32_t> max_single_image_height{};
  bool enabled{true};
  std::string unavailable_reason{};
  std::string license{};
  int default_speed{};
};

struct AvifEncoderSelectionRequest {
  AvifEncoderMode requested_encoder{AvifEncoderMode::automatic};
  ChromaMode requested_chroma{ChromaMode::auto_keep};
  std::optional<int> requested_bit_depth{};
  bool requested_bit_depth_explicit{true};
  std::string requested_bit_depth_reason{};
  bool has_alpha{};
  bool must_preserve_alpha{};
  bool visual_quality_search{};
  bool speed_explicit{};
  std::uint64_t pixel_count{};
  std::uint32_t width{};
  std::uint32_t height{};
  int speed{default_speed_for(OutputFormat::avif)};
};

struct AvifEncoderSelection {
  AvifEncoderMode requested_encoder{AvifEncoderMode::automatic};
  AvifEncoderMode applied_encoder{AvifEncoderMode::automatic};
  ChromaMode requested_chroma{ChromaMode::auto_keep};
  ChromaMode applied_chroma{ChromaMode::auto_keep};
  std::optional<int> requested_bit_depth{};
  std::optional<int> applied_bit_depth{};
  std::string bit_depth_reason{};
  std::uint64_t pixel_count{};
  int speed{};
  std::string license{};
  std::string fallback_reason{};
};

struct EncodeTimingDiagnostics {
  double decode_seconds{-1.0};
  double prepare_seconds{-1.0};
  double encode_seconds{-1.0};
  double avif_rgb_to_yuv_seconds{-1.0};
  double avif_add_image_seconds{-1.0};
  double avif_finish_seconds{-1.0};
  double avif_output_copy_seconds{-1.0};
  double write_seconds{-1.0};
  double visual_quality_search_seconds{-1.0};
  double visual_quality_candidate_encode_seconds{-1.0};
  double visual_quality_candidate_decode_seconds{-1.0};
  double visual_quality_candidate_io_seconds{-1.0};
  double visual_quality_luma_seconds{-1.0};
  double gmsd_seconds{-1.0};
  double ms_ssim_seconds{-1.0};
  double visual_quality_metric_seconds{-1.0};
  int visual_quality_candidate_count{};
  int visual_quality_decode_memory_fallback_count{};
  int visual_quality_gpu_fallback_count{};
};

struct EncodeDiagnostics {
  std::string decoder_id{};
  std::string encoder_id{};
  std::string requested_encoder_id{};
  std::string user_encoder_id{};
  std::string user_chroma{};
  std::string source_chroma{};
  std::string requested_chroma{};
  std::string applied_chroma{};
  std::string chroma_reason{};
  std::string requested_color_representation{};
  std::string applied_color_representation{};
  std::optional<int> source_bit_depth{};
  std::optional<int> requested_bit_depth{};
  std::optional<int> applied_bit_depth{};
  std::string bit_depth_reason{};
  std::string alpha_policy{};
  bool source_has_alpha_channel{};
  std::string source_alpha_mode{};
  std::optional<bool> has_non_opaque_alpha{};
  bool encoder_supports_alpha{};
  std::string applied_alpha{};
  std::string alpha_reason{};
  std::optional<int> source_color_primaries{};
  std::optional<int> source_transfer_characteristics{};
  std::optional<int> source_matrix_coefficients{};
  std::optional<int> source_color_range{};
  std::optional<HdrContentLightMetadata> source_content_light{};
  std::optional<int> applied_color_primaries{};
  std::optional<int> applied_transfer_characteristics{};
  std::optional<int> applied_matrix_coefficients{};
  std::optional<int> applied_color_range{};
  bool source_has_icc{};
  std::string applied_icc{};
  bool source_has_hdr_metadata{};
  std::string applied_hdr_metadata{};
  std::string color_metadata_source{};
  std::string color_reason{};
  std::string fallback_reason{};
  std::string encoder_license{};
  std::string integration_mode{};
  int jpegli_progressive_level{
      encoding_defaults::default_jpegli_progressive_level};
  bool jpegli_optimize_huffman{
      encoding_defaults::default_jpegli_optimize_huffman};
  bool jpegli_xyb{encoding_defaults::default_jpegli_xyb};
  SpeedMapping speed_mapping{};
  int encoder_threads{};
  std::uint64_t memory_budget_bytes{};
  bool used_decoder_fallback{};
  bool visual_quality_gpu_requested{};
  bool visual_quality_gpu_used{};
  std::string visual_quality_gpu_path{};
  std::string visual_quality_gpu_fallback_reason{};
  std::string visual_quality_search_trace{};
  EncodeTimingDiagnostics timing{};
};

struct NativeEncodeSettings {
  OutputFormat output_format{OutputFormat::avif};
  int quality{default_quality_for(output_format)};
  std::optional<int> visual_quality{};
  int speed{default_speed_for(output_format)};
  bool speed_explicit{};
  std::optional<int> bit_depth{};
  bool bit_depth_explicit{};
  ChromaMode chroma_mode{ChromaMode::auto_keep};
  AvifColorRepresentation avif_color_representation{
      AvifColorRepresentation::yuv};
  AvifEncoderMode avif_encoder{AvifEncoderMode::automatic};
  AlphaModePolicy alpha_policy{AlphaModePolicy::automatic};
  ChromaMode requested_chroma_mode{ChromaMode::auto_keep};
  AvifColorRepresentation requested_avif_color_representation{
      AvifColorRepresentation::yuv};
  AvifEncoderMode requested_avif_encoder{AvifEncoderMode::automatic};
  AlphaModePolicy requested_alpha_policy{AlphaModePolicy::automatic};
  std::optional<int> requested_bit_depth{};
  std::string user_encoder_id{};
  std::string user_chroma{};
  std::string source_chroma{};
  std::optional<int> source_bit_depth{};
  std::string chroma_reason{};
  std::string alpha_policy_name{};
  bool source_has_alpha_channel{};
  std::string source_alpha_mode{};
  std::optional<bool> has_non_opaque_alpha{};
  bool encoder_supports_alpha{};
  std::string applied_alpha{};
  std::string alpha_reason{};
  std::optional<int> source_color_primaries{};
  std::optional<int> source_transfer_characteristics{};
  std::optional<int> source_matrix_coefficients{};
  std::optional<int> source_color_range{};
  std::optional<HdrContentLightMetadata> source_content_light{};
  std::optional<int> applied_color_primaries{};
  std::optional<int> applied_transfer_characteristics{};
  std::optional<int> applied_matrix_coefficients{};
  std::optional<int> applied_color_range{};
  bool source_has_icc{};
  std::string applied_icc{};
  bool source_has_hdr_metadata{};
  std::string applied_hdr_metadata{};
  std::string color_metadata_source{};
  std::string color_reason{};
  std::string bit_depth_reason{};
  std::string encoder_fallback_reason{};
  bool strip_metadata{};
  bool visual_quality_fallback{};
  bool visual_quality_gpu{true};
  bool jxl_jpeg_lossless_candidate{};
  bool avif_tune_iq{encoding_defaults::default_avif_tune_iq};
  int jpegli_progressive_level{2};
  bool jpegli_optimize_huffman{true};
  bool jpegli_xyb{};
  ResourcePlan resources{};
  std::optional<GridPlan> avif_grid_plan{};
  std::span<const std::byte> jxl_rgb8_input{};
  std::span<const std::byte> jpegli_rgb8_input{};
};

struct NativeEncodeResult {
  EncodedImage encoded{};
  EncodeDiagnostics diagnostics{};
  int final_quality{};
  bool lossless{};
  bool visual_quality_target_met{true};
  int search_attempt_count{};
  std::optional<VisualScoreBreakdown> visual_score{};
  double raw_gmsd{};
  double raw_ms_ssim{};
};

EncodeDiagnostics diagnostics_from_settings(const NativeEncodeSettings& settings) {
  return EncodeDiagnostics{.user_encoder_id = settings.user_encoder_id,
                           .user_chroma = settings.user_chroma,
                           .source_chroma = settings.source_chroma,
                           .requested_chroma = chroma_mode_name(settings.requested_chroma_mode),
                           .applied_chroma = chroma_mode_name(settings.chroma_mode),
                           .chroma_reason = settings.chroma_reason,
                           .requested_color_representation =
                               avif_color_representation_name(
                                   settings.requested_avif_color_representation),
                           .applied_color_representation =
                               avif_color_representation_name(
                                   settings.avif_color_representation),
                           .source_bit_depth = settings.source_bit_depth,
                           .requested_bit_depth = settings.requested_bit_depth,
                           .applied_bit_depth = settings.bit_depth,
                           .bit_depth_reason = settings.bit_depth_reason,
                           .alpha_policy = settings.alpha_policy_name.empty()
                                               ? alpha_mode_policy_name(settings.requested_alpha_policy)
                                               : settings.alpha_policy_name,
                           .source_has_alpha_channel = settings.source_has_alpha_channel,
                           .source_alpha_mode = settings.source_alpha_mode,
                           .has_non_opaque_alpha = settings.has_non_opaque_alpha,
                           .encoder_supports_alpha = settings.encoder_supports_alpha,
                           .applied_alpha = settings.applied_alpha,
                           .alpha_reason = settings.alpha_reason,
                           .source_color_primaries = settings.source_color_primaries,
                           .source_transfer_characteristics = settings.source_transfer_characteristics,
                           .source_matrix_coefficients = settings.source_matrix_coefficients,
                           .source_color_range = settings.source_color_range,
                           .source_content_light = settings.source_content_light,
                           .applied_color_primaries = settings.applied_color_primaries,
                           .applied_transfer_characteristics = settings.applied_transfer_characteristics,
                           .applied_matrix_coefficients = settings.applied_matrix_coefficients,
                           .applied_color_range = settings.applied_color_range,
                           .source_has_icc = settings.source_has_icc,
                           .applied_icc = settings.applied_icc,
                           .source_has_hdr_metadata = settings.source_has_hdr_metadata,
                           .applied_hdr_metadata = settings.applied_hdr_metadata,
                           .color_metadata_source = settings.color_metadata_source,
                           .color_reason = settings.color_reason,
                           .fallback_reason = settings.encoder_fallback_reason,
                           .jpegli_progressive_level = settings.jpegli_progressive_level,
                           .jpegli_optimize_huffman = settings.jpegli_optimize_huffman,
                           .jpegli_xyb = settings.jpegli_xyb,
                           .visual_quality_gpu_requested = settings.visual_quality && settings.visual_quality_gpu,
                           .visual_quality_gpu_path = settings.visual_quality ? (settings.visual_quality_gpu ? "requested" : "cpu-disabled") : "not-requested"};
}

struct DecodeOptions {
  std::optional<bool> copy_metadata_payloads{};
};

class ImageDecoder {
 public:
  virtual ~ImageDecoder() = default;
  [[nodiscard]] virtual std::string_view id() const noexcept = 0;
  [[nodiscard]] virtual bool can_decode(const fs::path& path) const = 0;
  virtual std::expected<ImageDecodeResult, std::string> decode(
      const fs::path& path) const = 0;
  virtual std::expected<ImageDecodeResult, std::string> decode_memory(
      std::span<const std::byte>, std::string_view,
      DecodeOptions = {}) const {
    return std::unexpected{"该解码器不支持内存解码。"};
  }
  virtual std::expected<ImageDimensions, std::string> probe_dimensions(
      const fs::path& path) const {
    auto decoded = decode(path);
    if (!decoded) {
      return std::unexpected{decoded.error()};
    }
    if (decoded->image.width > std::numeric_limits<std::uint32_t>::max() ||
        decoded->image.height > std::numeric_limits<std::uint32_t>::max()) {
      return std::unexpected{"图片尺寸超过内部尺寸表示范围。"};
    }
    return make_image_dimensions(static_cast<std::uint32_t>(decoded->image.width),
                                 static_cast<std::uint32_t>(decoded->image.height));
  }
};

class ImageEncoder {
 public:
  virtual ~ImageEncoder() = default;
  [[nodiscard]] virtual std::string_view id() const noexcept = 0;
  [[nodiscard]] virtual CodecCapabilities capabilities() const = 0;
  virtual std::expected<NativeEncodeResult, std::string> encode(
      const ImageBuffer& image,
      const NativeEncodeSettings& settings,
      std::stop_token stop_token = {}) const = 0;
};

class CodecBackend {
 public:
  virtual ~CodecBackend() = default;
  [[nodiscard]] virtual BackendId backend_id() const noexcept = 0;
  virtual std::expected<NativeEncodeResult, std::string> convert(
      const fs::path& input,
      const NativeEncodeSettings& settings) const = 0;
};

SpeedMapping map_avif_speed_to_aom_cpu_used(int speed) {
  speed = std::clamp(speed, 0, 10);
  return SpeedMapping{.user_speed = speed,
                      .codec_value = speed,
                      .codec_key = "aom:cpu-used"};
}

SpeedMapping map_webp_speed_to_method(int speed) {
  speed = std::clamp(speed, 0, 10);
  const int method = std::clamp(6 - (speed * 6 + 5) / 10, 0, 6);
  return SpeedMapping{.user_speed = speed,
                      .codec_value = method,
                      .codec_key = "webp:method"};
}

SpeedMapping map_jxl_speed_to_effort(int speed) {
  speed = std::clamp(speed, 0, 10);
  const int effort = std::clamp(10 - speed, 1, 10);
  return SpeedMapping{.user_speed = speed,
                      .codec_value = effort,
                      .codec_key = "jxl:effort"};
}

SpeedMapping map_speed_for_format(OutputFormat format, int speed) {
  switch (format) {
    case OutputFormat::png:
      return SpeedMapping{.user_speed = -1, .codec_value = -1, .codec_key = ""};
    case OutputFormat::webp:
      return map_webp_speed_to_method(speed);
    case OutputFormat::jxl:
      return map_jxl_speed_to_effort(speed);
    case OutputFormat::jpgli:
      return SpeedMapping{.user_speed = -1, .codec_value = -1, .codec_key = ""};
    case OutputFormat::avif:
    default:
      return map_avif_speed_to_aom_cpu_used(speed);
  }
}

}  // namespace awj
