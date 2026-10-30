module;

#include <cstddef>
#include <array>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <memory>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

export module awj.image;

export namespace awj {

namespace fs = std::filesystem;

enum class PixelFormat {
  unknown,
  gray,
  rgb,
  rgba,
  yuv444,
  yuv422,
  yuv420,
};

enum class AlphaMode {
  none,
  straight,
  premultiplied,
};

// A 16-bit channel can be either a normalized integer sample or IEEE 754
// binary16.  scRGB arrives through WIC as the latter and must not be clipped
// before the actual SDR/HDR output path is chosen.
enum class SampleRepresentation {
  unorm,
  ieee_half_float,
};

enum class MetadataKind {
  icc,
  exif,
  xmp,
};

struct MetadataBlock {
  MetadataKind kind{};
  std::vector<std::byte> bytes{};
};

struct ImagePlane {
  std::vector<std::byte> bytes{};
  std::size_t stride{};
  // 外部输入只读；bytes 仍专用于拥有型输出。借用跨候选线程时共享真实所有者。
  std::span<const std::byte> borrowed{};
  std::shared_ptr<const void> lease{};

  [[nodiscard]] std::span<const std::byte> read_bytes() const noexcept {
    return borrowed.empty() ? std::span<const std::byte>{bytes} : borrowed;
  }
  [[nodiscard]] bool contains_rows(std::size_t rows, std::size_t row_bytes) const noexcept {
    if (!rows || stride < row_bytes || !stride ||
        rows - 1 > (std::numeric_limits<std::size_t>::max() - row_bytes) / stride) return false;
    return read_bytes().size() >= (rows - 1) * stride + row_bytes;
  }
};

struct HdrContentLightMetadata {
  std::uint16_t max_cll{};
  std::uint16_t max_pall{};
};

struct ImageSourceInfo {
  PixelFormat pixel_format{PixelFormat::unknown};
  int bit_depth{};
  std::optional<int> color_primaries{};
  std::optional<int> transfer_characteristics{};
  std::optional<int> matrix_coefficients{};
  std::optional<int> color_range{};
  std::optional<HdrContentLightMetadata> content_light{};
  bool has_hdr_metadata{};
  std::string color_metadata_source{};
  // Linear FP16 values are relative to this diffuse white, not necessarily scRGB's 80 nits.
  std::optional<float> linear_reference_white_nits{};
  bool source_has_gain_map{};
  // 与 AVChromaLocation 一致；未指定时保留原文件管线的居中采样约定。
  std::optional<int> chroma_location{};
};

// Precision of the current unorm samples, independent of their 8/16-bit storage.
// Pixel transforms must clear this unless they preserve channel sample values.
struct SignificantBits {
  int red{};
  int green{};
  int blue{};
  int alpha{};
};

struct ImageGeometry {
  std::size_t stored_width{}, stored_height{};
  std::size_t crop_x{}, crop_y{}, crop_width{}, crop_height{};
  unsigned rotation{}; // Counterclockwise quarter turns.
  int mirror_axis{-1};
};

struct ImageBuffer {
  std::size_t width{};
  std::size_t height{};
  PixelFormat pixel_format{PixelFormat::unknown};
  AlphaMode alpha_mode{AlphaMode::none};
  int bit_depth{8};
  SampleRepresentation sample_representation{SampleRepresentation::unorm};
  std::optional<ImageSourceInfo> source_info{};
  std::optional<SignificantBits> significant_bits{};
  std::optional<ImageGeometry> geometry{};
  std::vector<ImagePlane> planes{};
  std::vector<MetadataBlock> metadata{};

  [[nodiscard]] bool empty() const noexcept {
    return width == 0 || height == 0 || planes.empty();
  }
};

std::array<std::size_t, 2> image_storage_dimensions(const ImageBuffer& image) noexcept {
  return image.geometry ? std::array{image.geometry->stored_width, image.geometry->stored_height}
                        : std::array{image.width, image.height};
}

bool valid_image_geometry(const ImageBuffer& image) noexcept {
  if (!image.geometry) return true;
  const auto& g = *image.geometry;
  return image.width && image.height && image.width <= UINT32_MAX && image.height <= UINT32_MAX &&
      g.stored_width && g.stored_height && g.stored_width <= UINT32_MAX && g.stored_height <= UINT32_MAX &&
      g.crop_width && g.crop_height && g.crop_x < g.stored_width && g.crop_y < g.stored_height &&
      g.crop_width <= g.stored_width - g.crop_x && g.crop_height <= g.stored_height - g.crop_y &&
      g.rotation < 4 && g.mirror_axis >= -1 && g.mirror_axis <= 1;
}

std::array<std::size_t, 2> image_sample_coordinates(const ImageBuffer& image, std::size_t x, std::size_t y) noexcept {
  if (!image.geometry) return {x, y};
  const auto& g = *image.geometry;
  const auto width = g.rotation % 2 ? g.crop_height : g.crop_width;
  const auto height = g.rotation % 2 ? g.crop_width : g.crop_height;
  x = x * width / image.width; y = y * height / image.height;
  if (g.mirror_axis == 0) y = height - 1 - y;
  if (g.mirror_axis == 1) x = width - 1 - x;
  switch (g.rotation) {
    case 1: return {g.crop_x + g.crop_width - 1 - y, g.crop_y + x};
    case 2: return {g.crop_x + g.crop_width - 1 - x, g.crop_y + g.crop_height - 1 - y};
    case 3: return {g.crop_x + y, g.crop_y + g.crop_height - 1 - x};
    default: return {g.crop_x + x, g.crop_y + y};
  }
}

struct EncodedImage {
  std::vector<std::byte> bytes{};
  std::string codec_name{};
};

struct ImageDecodeResult {
  ImageBuffer image{};
  std::string decoder_id{};
  bool used_fallback{};
};

struct ImageWriteTarget {
  fs::path path{};
  bool atomic_replace{true};
};

}  // namespace awj
