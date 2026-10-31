#include <awj/awj.h>
#include <array>
#include <cstdio>
#include <memory>
#include <string_view>

int main() {
  if (awj_abi_version() != AWJ_ABI_VERSION) return 1;
#ifdef AWJ_EXPECTED_VERSION
  if (std::string_view(awj_version()) != AWJ_EXPECTED_VERSION) return 1;
#endif
  const std::array<uint8_t, 16> pixels{255,0,0,255, 0,255,0,255, 0,0,255,255, 255,255,255,255};
  awj_context* raw_context{};
  if (awj_context_create(nullptr, &raw_context) != AWJ_OK) return 1;
  std::unique_ptr<awj_context, decltype(&awj_context_destroy)> context(raw_context, awj_context_destroy);
  awj_frame_desc desc; awj_frame_desc_init(&desc);
  desc.width = desc.height = 2;
  desc.planes[0] = {pixels.data(), pixels.size(), 0, 8};
  desc.color.primaries = 1; desc.color.transfer = 13; desc.color.matrix = 0; desc.color.range = 1;
  awj_frame* raw_frame{};
  if (awj_frame_create(context.get(), &desc, nullptr, &raw_frame) != AWJ_OK) return 1;
  std::unique_ptr<awj_frame, decltype(&awj_frame_release)> frame(raw_frame, awj_frame_release);
  for (const auto codec : std::array<uint32_t, 5>{AWJ_CODEC_PNG, AWJ_CODEC_WEBP, AWJ_CODEC_JXL, AWJ_CODEC_JPEGLI, AWJ_CODEC_AVIF}) {
    awj_encode_options options; awj_encode_options_init(&options); options.codec = codec;
    awj_result* raw_result{};
    if (awj_encode(context.get(), frame.get(), &options, &raw_result) != AWJ_OK) {
      std::fprintf(stderr, "C++17 编码器 %u: %s\n", codec, awj_last_error()); return 1;
    }
    std::unique_ptr<awj_result, decltype(&awj_result_release)> result(raw_result, awj_result_release);
    if (!awj_result_data(result.get()) || awj_result_size(result.get()) == 0) return 1;
  }
  std::printf("C++17 consumer: five encoders through C ABI, version=%s\n", awj_version());
  return 0;
}
