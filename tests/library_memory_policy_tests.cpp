#include "../src/codecs/library_memory_policy.h"

#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <limits>
#include <memory>
#include <avif/avif.h>
#include <libheif/heif.h>
#include <libheif/heif_security.h>
#include <libraw/libraw.h>

namespace {
int fail(const char* text) {
  std::fputs(text, stderr);
  std::fputc('\n', stderr);
  return 1;
}
}

int main() {
  // The pinned libavif API does not implement a larger/disabled pixel limit.
  // Prove the remaining API boundary without decoding or allocating pixels.
  using AvifDecoder = std::unique_ptr<avifDecoder, decltype(&avifDecoderDestroy)>;
  AvifDecoder avif{avifDecoderCreate(), avifDecoderDestroy};
  if (!avif) return fail("Could not create a small AVIF decoder.");
  for (const auto value : {std::uint32_t{0}, std::numeric_limits<std::uint32_t>::max()}) {
    avif->imageSizeLimit = value;
    if (avifDecoderParse(avif.get()) != AVIF_RESULT_NOT_IMPLEMENTED)
      return fail("Locked libavif pixel-limit API changed; review the retained decoder boundary.");
  }
  avif->imageSizeLimit = AVIF_DEFAULT_IMAGE_SIZE_LIMIT;
  if (avifDecoderParse(avif.get()) != AVIF_RESULT_IO_NOT_SET)
    return fail("Valid libavif API limit failed before input was configured.");
  using Context = std::unique_ptr<heif_context, decltype(&heif_context_free)>;
  Context finite{heif_context_alloc(), heif_context_free};
  Context unlocked{heif_context_alloc(), heif_context_free};
  if (!finite || !unlocked) return fail("Could not create small HEIF contexts.");
  auto* normal = heif_context_get_security_limits(finite.get());
  auto* unlimited = heif_context_get_security_limits(unlocked.get());
  if (!normal || !unlimited || unlimited->version < 4)
    return fail("Locked libheif lacks instance memory security limits.");
  const auto before = *normal;
  awj::library_memory_policy::apply_heif(*normal, false);
  awj::library_memory_policy::apply_heif(*unlimited, true);
  if (normal->max_memory_block_size != before.max_memory_block_size ||
      normal->max_total_memory != before.max_total_memory ||
      normal->max_color_profile_size != before.max_color_profile_size ||
      normal->max_image_size_pixels != before.max_image_size_pixels)
    return fail("HEIF finite context was changed by another context's opt-out.");
  if (unlimited->max_memory_block_size || unlimited->max_total_memory ||
      unlimited->max_color_profile_size || unlimited->max_image_size_pixels)
    return fail("HEIF instance retained a memory allocation quota.");
  if (unlimited->max_items != before.max_items ||
      unlimited->max_number_of_tiles != before.max_number_of_tiles ||
      unlimited->max_components != before.max_components ||
      unlimited->max_iloc_extents_per_item != before.max_iloc_extents_per_item ||
      unlimited->max_children_per_box != before.max_children_per_box ||
      unlimited->max_sequence_frames != before.max_sequence_frames ||
      unlimited->max_bayer_pattern_pixels != before.max_bayer_pattern_pixels ||
      unlimited->max_size_entity_group != before.max_size_entity_group ||
      unlimited->max_sample_description_box_entries != before.max_sample_description_box_entries ||
      unlimited->max_sample_group_description_box_entries != before.max_sample_group_description_box_entries ||
      unlimited->max_number_of_file_brands != before.max_number_of_file_brands ||
      unlimited->max_bad_pixels != before.max_bad_pixels ||
      unlimited->max_iso23001_17_pixel_size_bytes != before.max_iso23001_17_pixel_size_bytes ||
      unlimited->parent != before.parent)
    return fail("HEIF opt-out changed structure/format validation or parent accounting.");
  // LibRaw 实例自身较大，两个测试实例不占用 MSVC 的默认线程栈。
  auto bounded_raw = std::make_unique<LibRaw>();
  auto unlocked_raw = std::make_unique<LibRaw>();
  const auto raw_limit = bounded_raw->imgdata.rawparams.max_raw_memory_mb;
  awj::library_memory_policy::apply_libraw(bounded_raw->imgdata.rawparams, false);
  awj::library_memory_policy::apply_libraw(unlocked_raw->imgdata.rawparams, true);
  if (!raw_limit || bounded_raw->imgdata.rawparams.max_raw_memory_mb != raw_limit ||
      unlocked_raw->imgdata.rawparams.max_raw_memory_mb != std::numeric_limits<unsigned>::max())
    return fail("LibRaw instance limits were guessed as zero or shared across decoders.");
  // Every LibRaw dimension is bounded to 65535 in the locked unpacker. This
  // tests representability only; it does not allocate a large image.
  constexpr auto maximum_raw_image = std::uint64_t{65535} * 65535 * 4 * sizeof(float);
  if (std::uint64_t{unlocked_raw->imgdata.rawparams.max_raw_memory_mb} * 1024 * 1024 <= maximum_raw_image)
    return fail("LibRaw parameter ceiling is below its image representation boundary.");
  return 0;
}
