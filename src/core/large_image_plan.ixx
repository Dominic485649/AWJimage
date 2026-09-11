module;

#include <algorithm>
#include <cstdint>
#include <expected>
#include <format>
#include <limits>
#include <string>
#include <vector>

export module awj.large_image_plan;

import awj.encoding_defaults;

export namespace awj {

struct ImageDimensions {
  std::uint32_t width{};
  std::uint32_t height{};
  std::uint64_t pixel_count{};
};

enum class LargeImageClass { ordinary, ordinary_deferred_tail, large_mode_required };
enum class LargeImageReason { none, medium_pixels, pixel_limit_exceeded, dimension_limit_exceeded };

struct LargeImageDecision {
  LargeImageClass klass{LargeImageClass::ordinary};
  LargeImageReason reason{LargeImageReason::none};
  bool available_grid{};
  std::string reason_text{};
};

struct LargeImageLimits {
  std::uint32_t max_width{encoding_defaults::avif_single_image_max_dimension};
  std::uint32_t max_height{encoding_defaults::avif_single_image_max_dimension};
  std::uint64_t max_pixels{encoding_defaults::avif_single_image_max_pixels};
  std::string_view encoder_name{"AOM/libaom"};
};

inline constexpr LargeImageLimits aom_large_image_limits{};
enum class GridMode { auto_grid, manual_grid };

struct GridPlanRequest {
  std::uint32_t width{};
  std::uint32_t height{};
  GridMode mode{GridMode::auto_grid};
  std::uint32_t manual_cols{};
  std::uint32_t manual_rows{};
  bool clamped_padding_enabled{encoding_defaults::default_experimental_clamped_grid_padding};
};

struct GridPlan {
  std::uint32_t cols{};
  std::uint32_t rows{};
  std::uint32_t tile_width{};
  std::uint32_t tile_height{};
  std::uint32_t padded_width{};
  std::uint32_t padded_height{};
  bool uses_padding{};
  bool clamped_to_original_size{};
};

ImageDimensions make_image_dimensions(std::uint32_t width,
                                             std::uint32_t height) noexcept {
  return ImageDimensions{.width = width,
                         .height = height,
                         .pixel_count = static_cast<std::uint64_t>(width) *
                                        static_cast<std::uint64_t>(height)};
}

std::uint64_t decoded_rgba_bytes_for_dimensions(ImageDimensions dimensions) noexcept {
  constexpr std::uint64_t channels = 4;
  constexpr auto max_value = std::numeric_limits<std::uint64_t>::max();
  const auto width = static_cast<std::uint64_t>(dimensions.width);
  const auto height = static_cast<std::uint64_t>(dimensions.height);
  if (width == 0 || height == 0) {
    return 1;
  }
  if (width > max_value / height) {
    return max_value;
  }
  const auto pixels = width * height;
  if (pixels > max_value / channels) {
    return max_value;
  }
  return std::max<std::uint64_t>(1, pixels * channels);
}

std::uint64_t visual_quality_working_set_bytes_for_dimensions(
    ImageDimensions dimensions) noexcept {
  constexpr std::uint64_t multiplier = 7;
  const auto decoded_rgba_bytes = decoded_rgba_bytes_for_dimensions(dimensions);
  const auto max_value = std::numeric_limits<std::uint64_t>::max();
  if (decoded_rgba_bytes > max_value / multiplier) {
    return max_value;
  }
  return std::max<std::uint64_t>(1, decoded_rgba_bytes * multiplier);
}

std::uint64_t avif_encode_working_set_bytes_for_dimensions(
    ImageDimensions dimensions) noexcept {
  // ponytail: dimension-only upper estimate, not an allocator limit. Measured
  // 4096x4096 AOM 10-bit 444 used ~1.1 GiB/file; allow 128 B/pixel plus
  // fixed overhead for high-depth/alpha and encoder buffers. Refine from
  // broader codec measurements before allowing more concurrency.
  constexpr std::uint64_t multiplier = 32;
  constexpr std::uint64_t overhead = 32ull * 1024 * 1024;
  const auto decoded_rgba_bytes = decoded_rgba_bytes_for_dimensions(dimensions);
  const auto max_value = std::numeric_limits<std::uint64_t>::max();
  if (decoded_rgba_bytes > (max_value - overhead) / multiplier) {
    return max_value;
  }
  return decoded_rgba_bytes * multiplier + overhead;
}

LargeImageDecision classify_large_image(ImageDimensions dimensions,
                                         bool grid_available,
                                         LargeImageLimits limits = aom_large_image_limits) {
  const bool dimension_exceeded =
      dimensions.width > limits.max_width || dimensions.height > limits.max_height;
  const bool pixel_limit_exceeded =
      dimensions.pixel_count > limits.max_pixels;

  LargeImageDecision decision{.available_grid = grid_available};
  if (dimension_exceeded) {
    decision.klass = LargeImageClass::large_mode_required;
    decision.reason = LargeImageReason::dimension_limit_exceeded;
    decision.reason_text = std::format(
        "输入尺寸 {}x{} 超过 {} 单图上限 {}x{}，已转入自动大图链路。",
        dimensions.width, dimensions.height, limits.encoder_name,
        limits.max_width, limits.max_height);
    return decision;
  }
  if (pixel_limit_exceeded) {
    decision.klass = LargeImageClass::large_mode_required;
    decision.reason = LargeImageReason::pixel_limit_exceeded;
    decision.reason_text = std::format(
        "输入像素数 {} 超过 {} 单图上限 {}，已转入自动大图链路。",
        dimensions.pixel_count, limits.encoder_name, limits.max_pixels);
    return decision;
  }
  if (dimensions.pixel_count > encoding_defaults::ordinary_large_defer_min_pixels) {
    decision.klass = LargeImageClass::ordinary_deferred_tail;
    decision.reason = LargeImageReason::medium_pixels;
    decision.reason_text = std::format(
        "输入像素数 {} 大于 {} 且未超过 AVIF 单图上限 {}，将延后到普通队列末尾编码。",
        dimensions.pixel_count, encoding_defaults::ordinary_large_defer_min_pixels,
        limits.max_pixels);
    return decision;
  }
  decision.reason_text = "普通图片按原队列编码。";
  return decision;
}

std::string large_image_reason_name(LargeImageReason reason) {
  switch (reason) {
    case LargeImageReason::medium_pixels:
      return "medium_pixels";
    case LargeImageReason::pixel_limit_exceeded:
      return "pixel_limit_exceeded";
    case LargeImageReason::dimension_limit_exceeded:
      return "dimension_limit_exceeded";
    case LargeImageReason::none:
    default:
      return "none";
  }
}

namespace large_image_plan_detail {

std::uint32_t ceil_div(std::uint32_t value, std::uint32_t divisor) noexcept {
  return divisor == 0 ? 0u : value / divisor + (value % divisor == 0 ? 0u : 1u);
}

std::expected<std::uint32_t, std::string> next_divisible_partition(
    std::uint32_t dimension,
    std::uint32_t min_candidate,
    std::uint32_t max_candidate,
    std::string_view axis) {
  if (dimension == 0) {
    return std::unexpected{"grid 输入尺寸不能为 0。"};
  }
  const auto start = std::clamp(min_candidate, 1u, max_candidate);
  std::vector<unsigned char> impossible(static_cast<std::size_t>(max_candidate) + 1u);
  for (std::uint32_t candidate = start; candidate <= max_candidate; ++candidate) {
    if (impossible[candidate]) {
      continue;
    }
    if (dimension % candidate == 0) {
      return candidate;
    }
    for (std::uint32_t multiple = candidate; multiple <= max_candidate; multiple += candidate) {
      impossible[multiple] = 1;
    }
  }
  return std::unexpected{std::format(
      "{}方向无法在 {}..{} 内找到可整除的 grid 分割数。",
      axis, start, max_candidate)};
}

std::string indivisible_grid_error(std::uint32_t width,
                                   std::uint32_t height,
                                   std::uint32_t cols,
                                   std::uint32_t rows) {
  const bool width_bad = cols == 0 || width % cols != 0;
  const bool height_bad = rows == 0 || height % rows != 0;
  std::string axis = width_bad && height_bad ? "宽、高" : (width_bad ? "宽" : "高");
  return std::format(
      "grid 分割不可整除：当前宽高 {}x{}，cols={} rows={}，{}方向不能整除。请改用可整除分割数，或启用 padding 规划后在支持安全裁切的编码路径中处理。",
      width, height, cols, rows, axis);
}

std::expected<void, std::string> validate_grid_counts(std::uint32_t cols,
                                                       std::uint32_t rows) {
  if (cols == 0 || rows == 0) {
    return std::unexpected{"grid cols/rows 必须大于 0。"};
  }
  if (cols > encoding_defaults::grid_max_cols || rows > encoding_defaults::grid_max_rows) {
    return std::unexpected{std::format(
        "grid cols/rows 不能超过 {}x{}，当前 cols={} rows={}。",
        encoding_defaults::grid_max_cols, encoding_defaults::grid_max_rows, cols, rows)};
  }
  return {};
}

std::expected<GridPlan, std::string> make_plan(std::uint32_t width,
                                               std::uint32_t height,
                                               std::uint32_t cols,
                                               std::uint32_t rows,
                                               bool padding) {
  if (auto valid = validate_grid_counts(cols, rows); !valid) {
    return std::unexpected{valid.error()};
  }
  if (width == 0 || height == 0) {
    return std::unexpected{"grid 输入尺寸不能为 0。"};
  }
  if (!padding && (width % cols != 0 || height % rows != 0)) {
    return std::unexpected{indivisible_grid_error(width, height, cols, rows)};
  }
  const auto tile_width = padding ? ceil_div(width, cols) : width / cols;
  const auto tile_height = padding ? ceil_div(height, rows) : height / rows;
  const auto max_uint32 = std::numeric_limits<std::uint32_t>::max();
  if (tile_width > max_uint32 / cols || tile_height > max_uint32 / rows) {
    return std::unexpected{"grid padding 后尺寸超过运行时限制。"};
  }
  const auto padded_width = tile_width * cols;
  const auto padded_height = tile_height * rows;
  return GridPlan{.cols = cols,
                  .rows = rows,
                  .tile_width = tile_width,
                  .tile_height = tile_height,
                  .padded_width = padded_width,
                  .padded_height = padded_height,
                  .uses_padding = padded_width != width || padded_height != height,
                  .clamped_to_original_size = padding &&
                                              (padded_width != width || padded_height != height)};
}

}  // namespace large_image_plan_detail

std::expected<GridPlan, std::string> plan_grid(GridPlanRequest request) {
  if (request.width == 0 || request.height == 0) {
    return std::unexpected{"grid 输入尺寸不能为 0。"};
  }

  std::uint32_t cols{};
  std::uint32_t rows{};
  if (request.mode == GridMode::manual_grid) {
    cols = request.manual_cols;
    rows = request.manual_rows;
    return large_image_plan_detail::make_plan(
        request.width, request.height, cols, rows, request.clamped_padding_enabled);
  }

  cols = std::clamp(
      large_image_plan_detail::ceil_div(request.width, encoding_defaults::grid_auto_tile_width),
      1u, encoding_defaults::grid_max_cols);
  rows = std::clamp(
      large_image_plan_detail::ceil_div(request.height, encoding_defaults::grid_auto_tile_height),
      1u, encoding_defaults::grid_max_rows);

  auto divisible_cols = large_image_plan_detail::next_divisible_partition(
      request.width, cols, encoding_defaults::grid_max_cols, "宽");
  auto divisible_rows = large_image_plan_detail::next_divisible_partition(
      request.height, rows, encoding_defaults::grid_max_rows, "高");
  if (divisible_cols && divisible_rows) {
    return large_image_plan_detail::make_plan(
        request.width, request.height, *divisible_cols, *divisible_rows, false);
  }
  if (!request.clamped_padding_enabled) {
    return std::unexpected{large_image_plan_detail::indivisible_grid_error(
        request.width, request.height, cols, rows)};
  }

  return large_image_plan_detail::make_plan(
      request.width, request.height, cols, rows, true);
}

}  // namespace awj
