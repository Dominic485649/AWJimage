module;

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <fstream>
#include <format>
#include <istream>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

export module awj.raw_image_io;

import awj.core;
import awj.encoding_defaults;
import awj.image;
import awj.large_image_plan;

export namespace awj {

namespace raw_image_detail {

inline constexpr char magic[] = {'A', 'W', 'S', 'R', 'A', 'W', '1', '\0'};
// Windows Graphics Capture HDR frames are DXGI_FORMAT_R16G16B16A16_FLOAT:
// packed little-endian RGBA binary16 values in linear scRGB.
inline constexpr char wgc_scrgb_magic[] = {'A', 'W', 'J', 'W', 'G', 'C', '1', '\0'};
inline constexpr std::uint32_t pixel_format_rgba = 1;
inline constexpr std::uint32_t alpha_none = 0;
inline constexpr std::uint32_t alpha_straight = 1;
inline constexpr std::uint32_t alpha_premultiplied = 2;

struct Header {
  char magic[8]{};
  std::uint32_t width{};
  std::uint32_t height{};
  std::uint32_t pixel_format{};
  std::uint32_t alpha_mode{};
  std::uint32_t bit_depth{};
  std::uint32_t stride{};
  std::uint64_t byte_count{};
};

bool is_raw_magic(const Header& header) noexcept {
  return std::memcmp(header.magic, magic, sizeof(header.magic)) == 0;
}

bool is_wgc_scrgb_magic(const Header& header) noexcept {
  return std::memcmp(header.magic, wgc_scrgb_magic, sizeof(header.magic)) == 0;
}

std::uint32_t alpha_to_raw(AlphaMode mode) noexcept {
  switch (mode) {
    case AlphaMode::straight:
      return alpha_straight;
    case AlphaMode::premultiplied:
      return alpha_premultiplied;
    case AlphaMode::none:
    default:
      return alpha_none;
  }
}

AlphaMode alpha_from_raw(std::uint32_t value) noexcept {
  switch (value) {
    case alpha_straight:
      return AlphaMode::straight;
    case alpha_premultiplied:
      return AlphaMode::premultiplied;
    case alpha_none:
    default:
      return AlphaMode::none;
  }
}

bool alpha_mode_is_valid(std::uint32_t value) noexcept {
  return value == alpha_none || value == alpha_straight || value == alpha_premultiplied;
}

class UniqueFileHandle {
 public:
#ifdef _WIN32
  using native_handle = HANDLE;
  static constexpr native_handle invalid() noexcept { return INVALID_HANDLE_VALUE; }
#else
  using native_handle = int;
  static constexpr native_handle invalid() noexcept { return -1; }
#endif

  UniqueFileHandle() noexcept = default;
  explicit UniqueFileHandle(native_handle handle) noexcept : handle_{handle} {}
  ~UniqueFileHandle() { reset(); }
  UniqueFileHandle(const UniqueFileHandle&) = delete;
  UniqueFileHandle& operator=(const UniqueFileHandle&) = delete;
  UniqueFileHandle(UniqueFileHandle&& other) noexcept : handle_{other.release()} {}
  UniqueFileHandle& operator=(UniqueFileHandle&& other) noexcept {
    if (this != &other) {
      reset(other.release());
    }
    return *this;
  }

  [[nodiscard]] native_handle get() const noexcept { return handle_; }
  native_handle release() noexcept {
    const auto old = handle_;
    handle_ = invalid();
    return old;
  }
  void reset(native_handle handle = invalid()) noexcept {
#ifdef _WIN32
    if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
      CloseHandle(handle_);
    }
#else
    if (handle_ >= 0) {
      ::close(handle_);
    }
#endif
    handle_ = handle;
  }

 private:
  native_handle handle_{invalid()};
};

class OutputFileCleanup {
 public:
  explicit OutputFileCleanup(const fs::path& path) noexcept : path_{&path} {}
  ~OutputFileCleanup() { cleanup(); }
  OutputFileCleanup(const OutputFileCleanup&) = delete;
  OutputFileCleanup& operator=(const OutputFileCleanup&) = delete;

  void release() noexcept { active_ = false; }

 private:
  void cleanup() noexcept {
    if (!active_) {
      return;
    }
    try {
      std::error_code ec;
      fs::remove(*path_, ec);
    } catch (...) {
    }
  }

  const fs::path* path_{};
  bool active_{true};
};

std::expected<void, std::string> write_all(UniqueFileHandle::native_handle file,
                                           const void* data,
                                           std::uint64_t size,
                                           const fs::path& path) {
  auto remaining = size;
  const auto* cursor = static_cast<const std::byte*>(data);
  while (remaining > 0) {
#ifdef _WIN32
    const auto chunk = static_cast<DWORD>(
        std::min<std::uint64_t>(remaining, std::numeric_limits<DWORD>::max()));
    DWORD written = 0;
    if (!WriteFile(file, cursor, chunk, &written, nullptr)) {
      return std::unexpected{std::format("写入 raw 图像文件失败: {}；系统错误：{}",
                                         display_path_for_user(path),
                                         win32_error_message(GetLastError()))};
    }
#else
    const auto written = ::write(file, cursor, static_cast<std::size_t>(remaining));
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      return std::unexpected{std::format("写入 raw 图像文件失败: {}；系统错误：{}",
                                         display_path_for_user(path),
                                         posix_error_message(errno))};
    }
#endif
    if (written == 0) {
      return std::unexpected{std::format("写入 raw 图像文件失败: {}", display_path_for_user(path))};
    }
    cursor += written;
    remaining -= static_cast<std::uint64_t>(written);
  }
  return {};
}

}  // namespace raw_image_detail

std::expected<void, std::string> write_wgc_scrgb_half_stream_file(
    const fs::path& path, std::uint32_t width, std::uint32_t height,
    std::istream& input) {
  try {
    if (width == 0 || height == 0 ||
        width > std::numeric_limits<std::uint32_t>::max() / 8u ||
        width > std::numeric_limits<std::uint64_t>::max() / 8ull ||
        height > std::numeric_limits<std::uint64_t>::max() /
                     (static_cast<std::uint64_t>(width) * 8ull)) {
      return std::unexpected{"WGC raw 图像尺寸无效。"};
    }
    const auto stride = static_cast<std::uint64_t>(width) * 8ull;
    const auto byte_count = stride * height;
    if (byte_count > encoding_defaults::effective_max_input_file_bytes() -
                         sizeof(raw_image_detail::Header)) {
      return std::unexpected{"WGC raw 输入超过当前运行时上限。"};
    }
    raw_image_detail::Header header{};
    std::ranges::copy(raw_image_detail::wgc_scrgb_magic, header.magic);
    header.width = width;
    header.height = height;
    header.pixel_format = raw_image_detail::pixel_format_rgba;
    header.alpha_mode = raw_image_detail::alpha_none;
    header.bit_depth = 16;
    header.stride = static_cast<std::uint32_t>(stride);
    header.byte_count = byte_count;

    const auto parent = path.parent_path();
    std::error_code ec;
    if (!parent.empty()) {
      fs::create_directories(parent, ec);
      if (ec) return std::unexpected{"无法创建 WGC raw 临时目录。"};
    }
#ifdef _WIN32
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
      return std::unexpected{"无法创建 WGC raw 临时文件。"};
    }
#else
    const int file = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (file < 0) return std::unexpected{"无法创建 WGC raw 临时文件。"};
#endif
    raw_image_detail::OutputFileCleanup cleanup{path};
    raw_image_detail::UniqueFileHandle output{file};
    if (auto written = raw_image_detail::write_all(output.get(), &header, sizeof(header), path);
        !written) {
      return std::unexpected{written.error()};
    }
    std::array<char, 64 * 1024> buffer{};
    auto remaining = byte_count;
    while (remaining != 0) {
      const auto want = static_cast<std::streamsize>(
          std::min<std::uint64_t>(remaining, buffer.size()));
      input.read(buffer.data(), want);
      const auto received = input.gcount();
      if (received != want) {
        return std::unexpected{"WGC stdin 数据短于声明的 16-bit RGBA 帧。"};
      }
      if (auto written = raw_image_detail::write_all(
              output.get(), buffer.data(), static_cast<std::uint64_t>(received),
              path);
          !written) {
        return std::unexpected{written.error()};
      }
      remaining -= static_cast<std::uint64_t>(received);
    }
    char extra{};
    if (input.get(extra)) {
      return std::unexpected{"WGC stdin 数据长于声明的单帧，已拒绝。"};
    }
    if (!input.eof()) return std::unexpected{"读取 WGC stdin 失败。"};
#ifdef _WIN32
    if (!FlushFileBuffers(output.get())) {
      return std::unexpected{"刷新 WGC raw 临时文件失败。"};
    }
    const HANDLE output_handle = output.get();
    if (!CloseHandle(output_handle)) {
      return std::unexpected{"关闭 WGC raw 临时文件失败。"};
    }
#else
    const int output_handle = output.get();
    if (::fsync(output_handle) != 0 || ::close(output_handle) != 0) {
      return std::unexpected{"关闭 WGC raw 临时文件失败。"};
    }
#endif
    output.release();
    cleanup.release();
    return {};
  } catch (const std::bad_alloc&) {
    return std::unexpected{"WGC raw stdin 内存不足。"};
  } catch (const std::filesystem::filesystem_error&) {
    return std::unexpected{"WGC raw stdin 文件系统访问失败。"};
  }
}

std::expected<void, std::string> write_raw_image_file(
    const fs::path& path,
    const ImageBuffer& image) {
  try {
    if (image.pixel_format != PixelFormat::rgba || image.planes.empty()) {
      return std::unexpected{"raw 图像写入需要 RGBA ImageBuffer。"};
    }
    const auto& plane = image.planes.front();
    if (image.width == 0 || image.height == 0 || plane.stride == 0 ||
        plane.bytes.empty()) {
      return std::unexpected{"raw 图像写入收到空图像。"};
    }
    if (image.width > std::numeric_limits<std::uint32_t>::max() ||
        image.height > std::numeric_limits<std::uint32_t>::max() ||
        plane.stride > std::numeric_limits<std::uint32_t>::max() ||
        plane.stride > std::numeric_limits<std::uint64_t>::max() / image.height) {
      return std::unexpected{"raw 图像尺寸超过文件格式限制。"};
    }
    if (image.bit_depth != 8 && image.bit_depth != 10 && image.bit_depth != 12 &&
        image.bit_depth != 16) {
      return std::unexpected{"raw 图像写入收到不支持的位深。"};
    }
    const auto bytes_per_sample = image.bit_depth > 8 ? 2ull : 1ull;
    if (image.width > std::numeric_limits<std::uint64_t>::max() / 4ull / bytes_per_sample) {
      return std::unexpected{"raw 图像尺寸超过文件格式限制。"};
    }
    const auto min_stride = static_cast<std::uint64_t>(image.width) * 4ull * bytes_per_sample;
    if (plane.stride < min_stride) {
      return std::unexpected{"raw 图像 stride 小于像素格式要求。"};
    }
    const auto min_bytes = static_cast<std::uint64_t>(plane.stride) * image.height;
    if (plane.bytes.size() < min_bytes) {
      return std::unexpected{"raw 图像 payload 小于尺寸要求。"};
    }
    if (min_bytes > encoding_defaults::effective_max_input_file_bytes() - sizeof(raw_image_detail::Header)) {
      return std::unexpected{"raw 图像输出超过当前运行时上限。"};
    }
    raw_image_detail::Header header{};
    std::ranges::copy(raw_image_detail::magic, header.magic);
    header.width = static_cast<std::uint32_t>(image.width);
    header.height = static_cast<std::uint32_t>(image.height);
    header.pixel_format = raw_image_detail::pixel_format_rgba;
    header.alpha_mode = raw_image_detail::alpha_to_raw(image.alpha_mode);
    header.bit_depth = static_cast<std::uint32_t>(image.bit_depth);
    header.stride = static_cast<std::uint32_t>(plane.stride);
    header.byte_count = min_bytes;

    const auto parent = path.parent_path();
    std::error_code ec;
    if (!parent.empty()) {
      fs::create_directories(parent, ec);
      if (ec) {
        return std::unexpected{std::format("无法创建 raw 图像目录: {}；系统错误：{}",
                                           display_path_for_user(parent), ec.message())};
      }
    }
#ifdef _WIN32
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
      const auto error = GetLastError();
      if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) {
        return std::unexpected{std::format("raw 图像文件已存在: {}", display_path_for_user(path))};
      }
      return std::unexpected{std::format("无法创建 raw 图像文件: {}；系统错误：{}",
                                         display_path_for_user(path),
                                         win32_error_message(error))};
    }
#else
    const int file = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
    if (file < 0) {
      const int error = errno;
      if (error == EEXIST) {
        return std::unexpected{std::format("raw 图像文件已存在: {}", display_path_for_user(path))};
      }
      return std::unexpected{std::format("无法创建 raw 图像文件: {}；系统错误：{}",
                                         display_path_for_user(path),
                                         posix_error_message(error))};
    }
#endif
    raw_image_detail::OutputFileCleanup cleanup{path};
    raw_image_detail::UniqueFileHandle output{file};
    if (auto written = raw_image_detail::write_all(output.get(), &header, sizeof(header), path); !written) {
      return std::unexpected{written.error()};
    }
    if (auto written = raw_image_detail::write_all(output.get(), plane.bytes.data(), min_bytes, path); !written) {
      return std::unexpected{written.error()};
    }
#ifdef _WIN32
    if (!FlushFileBuffers(output.get())) {
      return std::unexpected{std::format("刷新 raw 图像文件失败: {}；系统错误：{}",
                                         display_path_for_user(path),
                                         win32_error_message(GetLastError()))};
    }
    const HANDLE output_handle = output.get();
    if (!CloseHandle(output_handle)) {
      return std::unexpected{std::format("关闭 raw 图像文件失败: {}；系统错误：{}",
                                         display_path_for_user(path),
                                         win32_error_message(GetLastError()))};
    }
#else
    const int output_handle = output.get();
    if (::fsync(output_handle) != 0) {
      return std::unexpected{std::format("刷新 raw 图像文件失败: {}；系统错误：{}",
                                         display_path_for_user(path),
                                         posix_error_message(errno))};
    }
    if (::close(output_handle) != 0) {
      return std::unexpected{std::format("关闭 raw 图像文件失败: {}；系统错误：{}",
                                         display_path_for_user(path),
                                         posix_error_message(errno))};
    }
#endif
    output.release();
    cleanup.release();
    return {};
  } catch (const std::bad_alloc&) {
    return std::unexpected{"raw 图像写入内存不足。"};
  } catch (const std::length_error&) {
    return std::unexpected{"raw 图像写入数据超过运行时限制。"};
  } catch (const std::filesystem::filesystem_error&) {
    return std::unexpected{"raw 图像写入文件系统访问失败。"};
  }
}

std::expected<ImageDimensions, std::string> probe_raw_image_dimensions(const fs::path& path) {
  try {
    std::ifstream input{path, std::ios::binary};
    if (!input) {
      return std::unexpected{std::format("无法读取 raw 图像文件: {}", display_path_for_user(path))};
    }
    raw_image_detail::Header header{};
    input.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!input || (!raw_image_detail::is_raw_magic(header) &&
                   !raw_image_detail::is_wgc_scrgb_magic(header))) {
      return std::unexpected{"raw 图像文件头无效。"};
    }
    if (header.pixel_format != raw_image_detail::pixel_format_rgba ||
        (header.bit_depth != 8 && header.bit_depth != 10 && header.bit_depth != 12 &&
         header.bit_depth != 16) ||
        !raw_image_detail::alpha_mode_is_valid(header.alpha_mode)) {
      return std::unexpected{"raw 图像格式不受支持。"};
    }
    if (header.width == 0 || header.height == 0 || header.stride == 0 ||
        header.width > std::numeric_limits<std::uint64_t>::max() / 4ull /
                           (header.bit_depth > 8 ? 2ull : 1ull)) {
      return std::unexpected{"raw 图像尺寸无效。"};
    }
    const auto expected_min_stride = static_cast<std::uint64_t>(header.width) * 4ull *
                                     (header.bit_depth > 8 ? 2ull : 1ull);
    if (header.stride < expected_min_stride ||
        header.height > std::numeric_limits<std::uint64_t>::max() / header.stride) {
      return std::unexpected{"raw 图像尺寸无效。"};
    }
    const auto min_bytes = static_cast<std::uint64_t>(header.stride) * header.height;
    if (header.byte_count != min_bytes) {
      return std::unexpected{"raw 图像 byte count 无效。"};
    }
    input.seekg(0, std::ios::end);
    if (!input) {
      return std::unexpected{"读取 raw 图像文件大小失败。"};
    }
    const auto file_size = input.tellg();
    if (file_size < 0) {
      return std::unexpected{"读取 raw 图像文件大小失败。"};
    }
    const auto file_size_bytes = static_cast<std::uint64_t>(file_size);
    if (file_size_bytes > encoding_defaults::effective_max_input_file_bytes()) {
      return std::unexpected{std::format("raw 图像文件超过当前输入上限: {}",
                                         display_path_for_user(path))};
    }
    if (file_size_bytes < sizeof(raw_image_detail::Header) ||
        file_size_bytes - sizeof(raw_image_detail::Header) != header.byte_count) {
      return std::unexpected{"raw 图像文件大小与 header 不一致。"};
    }
    return make_image_dimensions(header.width, header.height);
  } catch (const std::bad_alloc&) {
    return std::unexpected{"raw 图像尺寸探测内存不足。"};
  } catch (const std::length_error&) {
    return std::unexpected{"raw 图像尺寸探测数据超过运行时限制。"};
  } catch (const std::filesystem::filesystem_error&) {
    return std::unexpected{"raw 图像尺寸探测文件系统访问失败。"};
  }
}

std::expected<ImageBuffer, std::string> read_raw_image_file(const fs::path& path) {
  try {
    std::ifstream input{path, std::ios::binary};
    if (!input) {
      return std::unexpected{std::format("无法读取 raw 图像文件: {}", display_path_for_user(path))};
    }
    raw_image_detail::Header header{};
    input.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!input || (!raw_image_detail::is_raw_magic(header) &&
                   !raw_image_detail::is_wgc_scrgb_magic(header))) {
      return std::unexpected{"raw 图像文件头无效。"};
    }
    if (header.pixel_format != raw_image_detail::pixel_format_rgba ||
        (header.bit_depth != 8 && header.bit_depth != 10 && header.bit_depth != 12 &&
         header.bit_depth != 16) ||
        !raw_image_detail::alpha_mode_is_valid(header.alpha_mode)) {
      return std::unexpected{"raw 图像格式不受支持。"};
    }
    if (header.width == 0 || header.height == 0 || header.stride == 0 ||
        header.width > std::numeric_limits<std::uint64_t>::max() / 4ull /
                           (header.bit_depth > 8 ? 2ull : 1ull)) {
      return std::unexpected{"raw 图像尺寸无效。"};
    }
    const auto expected_min_stride = static_cast<std::uint64_t>(header.width) * 4ull *
                                     (header.bit_depth > 8 ? 2ull : 1ull);
    if (header.width == 0 || header.height == 0 || header.stride < expected_min_stride ||
        header.height > std::numeric_limits<std::uint64_t>::max() / header.stride) {
      return std::unexpected{"raw 图像尺寸无效。"};
    }
    const auto min_bytes = static_cast<std::uint64_t>(header.stride) * header.height;
    if (header.byte_count < min_bytes || header.byte_count != min_bytes ||
        header.byte_count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()) ||
        header.byte_count > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max())) {
      return std::unexpected{"raw 图像 byte count 无效。"};
    }
    input.seekg(0, std::ios::end);
    if (!input) {
      return std::unexpected{"读取 raw 图像文件大小失败。"};
    }
    const auto file_size = input.tellg();
    if (file_size < 0) {
      return std::unexpected{"读取 raw 图像文件大小失败。"};
    }
    const auto file_size_bytes = static_cast<std::uint64_t>(file_size);
    if (file_size_bytes > encoding_defaults::effective_max_input_file_bytes()) {
      return std::unexpected{std::format("raw 图像文件超过当前输入上限: {}",
                                         display_path_for_user(path))};
    }
    if (file_size_bytes < sizeof(raw_image_detail::Header) ||
        file_size_bytes - sizeof(raw_image_detail::Header) != header.byte_count) {
      return std::unexpected{"raw 图像文件大小与 header 不一致。"};
    }
    input.seekg(static_cast<std::streamoff>(sizeof(raw_image_detail::Header)), std::ios::beg);
    if (!input) {
      return std::unexpected{"定位 raw 图像 payload 失败。"};
    }
    ImagePlane plane{.stride = header.stride};
    try {
      plane.bytes.resize(static_cast<std::size_t>(header.byte_count));
    } catch (const std::bad_alloc&) {
      return std::unexpected{"raw 图像读取 payload 内存不足。"};
    } catch (const std::length_error&) {
      return std::unexpected{"raw 图像读取 payload 尺寸超过运行时限制。"};
    }
    input.read(reinterpret_cast<char*>(plane.bytes.data()),
               static_cast<std::streamsize>(plane.bytes.size()));
    if (!input) {
      return std::unexpected{"读取 raw 图像 payload 失败。"};
    }
    const bool wgc_scrgb = raw_image_detail::is_wgc_scrgb_magic(header);
    ImageBuffer image{.width = header.width,
                      .height = header.height,
                      .pixel_format = PixelFormat::rgba,
                      .alpha_mode = raw_image_detail::alpha_from_raw(header.alpha_mode),
                      .bit_depth = static_cast<int>(header.bit_depth),
                      .sample_representation = wgc_scrgb
                                                   ? SampleRepresentation::ieee_half_float
                                                   : SampleRepresentation::unorm,
                      .source_info = wgc_scrgb
                                         ? std::optional<ImageSourceInfo>{ImageSourceInfo{
                                               .pixel_format = PixelFormat::rgba,
                                               .bit_depth = 16,
                                               .color_primaries = 1,
                                               .transfer_characteristics = 8,
                                               .matrix_coefficients = 0,
                                               .color_range = 1,
                                               .has_hdr_metadata = true,
                                               .color_metadata_source =
                                                   "wgc-scrgb-half-linear"}}
                                         : std::optional<ImageSourceInfo>{ImageSourceInfo{
                                               .pixel_format = PixelFormat::rgba,
                                               .bit_depth = static_cast<int>(header.bit_depth)}}};
    try {
      image.planes.push_back(std::move(plane));
    } catch (const std::bad_alloc&) {
      return std::unexpected{"raw 图像读取 plane list 内存不足。"};
    } catch (const std::length_error&) {
      return std::unexpected{"raw 图像读取 plane list 尺寸超过运行时限制。"};
    }
    return image;
  } catch (const std::bad_alloc&) {
    return std::unexpected{"raw 图像读取内存不足。"};
  } catch (const std::length_error&) {
    return std::unexpected{"raw 图像读取数据超过运行时限制。"};
  } catch (const std::filesystem::filesystem_error&) {
    return std::unexpected{"raw 图像读取文件系统访问失败。"};
  }
}

}  // namespace awj
