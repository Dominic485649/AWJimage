module;

#include "memory_policy.h"
#include <cstddef>
#include <cstdint>
#include <string_view>

export module awj.encoding_defaults;

export namespace awj::encoding_defaults {

inline constexpr std::string_view default_input_path_text = "input";
inline constexpr std::wstring_view default_input_path = L"input";
inline constexpr std::string_view default_output_template_text = "{name}";
inline constexpr std::wstring_view default_output_template = L"{name}";
inline constexpr std::string_view default_memory_limit_text = "auto";

inline constexpr int default_avif_quality = 70;
inline constexpr int default_webp_quality = 95;
inline constexpr int default_jxl_quality = 85;
inline constexpr int default_jpegli_quality = 90;
inline constexpr int default_webp_bit_depth = 8;
inline constexpr int default_quality = default_avif_quality;
inline constexpr int default_visual_quality = 90;

// 内置默认不是一个可选的命名预设：它只是所有未显式选择用户 JSONC 预设时的
// 起点。保留单一常量，避免旧 fast/balanced/best/extreme 名称继续渗入 CLI/UI。
inline constexpr int default_encode_timeout_minutes = 20;

inline constexpr std::uint64_t default_memory_limit_bytes = 0;
inline constexpr bool default_allow_wic_fallback = true;
inline constexpr bool default_visual_quality_fallback = true;

inline constexpr int default_aom_cpu_used = 5;
inline constexpr int default_native_speed = 5;
// AVIF 的 AOM cpu-used 仍可由编码器专用选项控制；通用 speed 默认和其它
// native 编码路径一致，避免 UI 留空时悄悄落到旧的 6。
inline constexpr int default_avif_native_speed = default_native_speed;
inline constexpr int default_webp_native_speed = 4;
inline constexpr int default_jxl_native_speed = 6;
inline constexpr int default_jpegli_native_speed = 5;
inline constexpr int default_jxl_effort = 4;
inline constexpr bool default_jxl_effort_is_explicit = false;
inline constexpr int default_jpegli_progressive_level = 2;
inline constexpr bool default_jpegli_optimize_huffman = true;
inline constexpr bool default_jpegli_xyb = false;

using MemoryPolicy = memory_policy::Policy;
using ScopedMemoryPolicy = memory_policy::Scope;
using PhysicalMemoryStatus = memory_policy::MemoryStatus;
inline constexpr auto memory_policy_unavailable_message = memory_policy::unavailable_message;

inline PhysicalMemoryStatus physical_memory_status() noexcept {
  return memory_policy::current_memory_status();
}
inline constexpr std::uint64_t physical_input_limit(std::uint64_t total) noexcept {
  return memory_policy::physical_input_limit(total);
}
inline auto resolve_memory_policy(bool unlock_memory, bool unlock_input,
                                  std::uint64_t physical_total) noexcept {
  return memory_policy::resolve(unlock_memory, unlock_input, physical_total);
}
inline auto system_memory_policy(bool unlock_memory = false, bool unlock_input = false) noexcept {
  return memory_policy::resolve_system(unlock_memory, unlock_input);
}
inline MemoryPolicy current_memory_policy() { return memory_policy::current(); }
inline std::uint64_t effective_max_input_file_bytes() {
  return current_memory_policy().input_limit();
}
inline std::size_t effective_codec_metadata_max_bytes(std::size_t overhead = 0) noexcept {
  return memory_policy::metadata_limit(overhead);
}
inline bool memory_limits_unlocked() noexcept { return memory_policy::unrestricted_memory(); }
inline constexpr bool byte_sum_fits(std::uint64_t used, std::uint64_t extra,
                                    std::uint64_t limit) noexcept {
  return memory_policy::fits_sum(used, extra, limit);
}

inline constexpr std::uint32_t avif_single_image_max_dimension = 65'536u;
inline constexpr std::uint64_t avif_single_image_max_pixels = 1ull << 30;
inline constexpr std::uint32_t grid_auto_tile_width = 16'384u;
inline constexpr std::uint32_t grid_auto_tile_height = 8'704u;
inline constexpr std::uint32_t grid_max_cols = 256u;
inline constexpr std::uint32_t grid_max_rows = 256u;
inline constexpr bool default_experimental_clamped_grid_padding = true;
inline constexpr int default_grid_overlap_pixels = 0;
inline constexpr int max_automatic_thread_budget = 128;

// Finite-mode default only. Runtime codec checks use the scoped getter above.
inline constexpr std::size_t codec_metadata_max_bytes = memory_policy::default_metadata_limit;
inline constexpr std::size_t jpeg_max_saved_metadata_marker_count = 4096;
inline constexpr std::size_t jpeg_marker_payload_max_bytes = 0xFFFFu - 2u;
inline constexpr std::size_t png_max_cached_metadata_chunks = 64;
inline constexpr std::size_t wic_max_color_contexts = 16;

inline constexpr std::size_t jxl_min_basic_info_probe_bytes = 16 * 1024;
inline constexpr std::size_t jxl_max_basic_info_probe_bytes = 1024 * 1024;
inline constexpr std::size_t jxl_min_encoder_output_buffer_bytes = 16 * 1024;
inline constexpr std::size_t jxl_max_initial_encoder_output_buffer_bytes =
    1024 * 1024;
inline constexpr std::size_t jxl_max_metadata_box_count = 16;

inline constexpr std::size_t webp_min_encoder_output_capacity = 16 * 1024;
inline constexpr std::size_t webp_max_initial_encoder_output_capacity =
    1024 * 1024;

inline constexpr int max_output_temp_path_attempts = 1000;
inline constexpr std::size_t output_copy_buffer_bytes = 4 * 1024 * 1024;

inline constexpr double visual_gmsd_best = 0.0025;
inline constexpr double visual_gmsd_worst = 0.60;
inline constexpr double visual_gmsd_curve_gamma = 0.70;
inline constexpr double visual_msssim_best = 0.9998;
inline constexpr double visual_msssim_worst = 0.995;
inline constexpr double visual_msssim_curve_gamma = 1.0;
inline constexpr double visual_gmsd_weight = 0.90;
inline constexpr double visual_msssim_weight = 0.10;
inline constexpr int visual_encoder_quality_min = 1;
inline constexpr int visual_encoder_quality_max = 100;
inline constexpr double visual_search_range_max = 34.0;
inline constexpr double visual_search_range_min = 5.0;
inline constexpr double visual_search_center_bias = 5.0;
inline constexpr double visual_search_curve_gamma = 1.28;

inline constexpr bool default_avif_tune_iq = true;

}  // namespace awj::encoding_defaults
