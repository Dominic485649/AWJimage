module;

#include <expected>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>

export module awj.avif_aom_codec;

import awj.codec;
import awj.animation;
import awj.config;
import awj.image;

export namespace awj {

bool avif_libavif_encoder_available(AvifEncoderMode mode);
bool avif_dav1d_decoder_available() noexcept;

std::vector<AvifEncoderCapability> avif_encoder_capabilities_for_current_build();

std::expected<AvifEncoderSelection, std::string> select_avif_encoder_for_current_build(
    const AvifEncoderSelectionRequest& request);

std::expected<NativeEncodeResult, std::string> encode_with_current_settings(
    const ImageBuffer& image,
    const NativeEncodeSettings& settings,
    std::stop_token stop_token = {});

std::unique_ptr<ImageDecoder> make_avif_image_decoder(int decode_threads);
std::expected<bool, std::string> avif_has_sequence(const std::filesystem::path& path);
std::expected<std::unique_ptr<AnimationReader>, std::string> open_avif_animation(
    const std::filesystem::path& path, std::uint64_t budget, std::stop_token stop);
std::expected<NativeEncodeResult, std::string> encode_avif_animation(
    AnimationReader& reader, const NativeEncodeSettings& settings,
    std::stop_token stop_token = {});
std::expected<ImageBuffer, std::string> parse_avif_container_info(const std::filesystem::path& path);
std::unique_ptr<ImageEncoder> make_avif_image_encoder(AvifEncoderMode mode);

}  // namespace awj
