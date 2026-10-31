module;
#include "../compat/stdlib_compat.h"

#include <filesystem>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>

export module awj.raw_codec;

import awj.codec;
import awj.decoder_common;
import awj.image;
import awj.large_image_plan;
import awj.raw_image_io;

export namespace awj {

class RawImageDecoder final : public ImageDecoder {
 public:
  [[nodiscard]] std::string_view id() const noexcept override { return "awj-raw"; }

  [[nodiscard]] bool can_decode(const fs::path& path) const override {
    static constexpr std::wstring_view extensions[] = {L".awsraw"};
    return decoder_common::extension_is_one_of(path, extensions);
  }

  awj::compat::expected<ImageDimensions, std::string> probe_dimensions(
      const fs::path& path) const override {
    auto dimensions = probe_raw_image_dimensions(path);
    if (!dimensions) {
      return awj::compat::unexpected{dimensions.error()};
    }
    return *dimensions;
  }

  awj::compat::expected<ImageDecodeResult, std::string> decode(const fs::path& path) const override {
    try {
      auto image = read_raw_image_file(path);
      if (!image) {
        return awj::compat::unexpected{image.error()};
      }
      return ImageDecodeResult{.image = std::move(*image), .decoder_id = "awj-raw"};
    } catch (const std::bad_alloc&) {
      return awj::compat::unexpected{"raw 图像解码内存不足。"};
    } catch (const std::length_error&) {
      return awj::compat::unexpected{"raw 图像解码数据超过运行时限制。"};
    } catch (const std::filesystem::filesystem_error&) {
      return awj::compat::unexpected{"raw 图像解码文件系统访问失败。"};
    }
  }
};

}  // namespace awj
