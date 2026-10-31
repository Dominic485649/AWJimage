module;
#include "../compat/stdlib_compat.h"

#include <filesystem>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>

export module awj.jxr_codec;

import awj.codec;
import awj.decoder_common;
import awj.image;
import awj.large_image_plan;
import awj.wic_codec;

export namespace awj {

class JxrImageDecoder final : public ImageDecoder {
 public:
  [[nodiscard]] std::string_view id() const noexcept override { return "windows-jxr"; }

  [[nodiscard]] bool can_decode(const fs::path& path) const override {
    static constexpr std::wstring_view extensions[] = {L".jxr", L".wdp", L".hdp"};
    return decoder_common::extension_is_one_of(path, extensions);
  }

  awj::compat::expected<ImageDimensions, std::string> probe_dimensions(
      const fs::path& path) const override {
    return WicImageDecoder{}.probe_dimensions(path);
  }

  awj::compat::expected<ImageDecodeResult, std::string> decode(const fs::path& path) const override {
    auto decoded = WicImageDecoder{}.decode(path);
    if (!decoded) {
      return awj::compat::unexpected{decoded.error()};
    }
    decoded->decoder_id = "windows-jxr";
    decoded->used_fallback = false;
    return decoded;
  }
};

}  // namespace awj
