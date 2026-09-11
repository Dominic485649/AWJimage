#include <cstdio>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

import awj.avif_registry;
import awj.codec;
import awj.config;
import awj.encoding_defaults;
import awj.large_image_plan;

void check(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}

int main() try {
  const auto capabilities = awj::avif_encoder_capabilities_for_build(true);
  check(capabilities.size() == 1 && capabilities.front().mode == awj::AvifEncoderMode::aom,
        "Only AOM may be registered");
  for (const auto mode : {awj::AvifEncoderMode::automatic, awj::AvifEncoderMode::aom}) {
    for (const auto chroma : {awj::ChromaMode::yuv420, awj::ChromaMode::yuv422, awj::ChromaMode::yuv444}) {
      for (const int depth : {8, 10, 12}) {
        for (const bool alpha : {false, true}) {
          const auto selected = awj::select_avif_encoder_from_capabilities({
              .requested_encoder = mode, .requested_chroma = chroma,
              .requested_bit_depth = depth, .has_alpha = alpha, .must_preserve_alpha = alpha,
              .speed_explicit = true, .pixel_count = 1024, .width = 32, .height = 32, .speed = 7}, capabilities);
          check(selected && selected->applied_encoder == awj::AvifEncoderMode::aom &&
                selected->applied_chroma == chroma && selected->applied_bit_depth == depth && selected->speed == 7,
                "Selection changed explicit depth/chroma/alpha/speed semantics");
          const auto diagnostics = awj::diagnostics_from_avif_selection(*selected);
          check(diagnostics.encoder_id == "aom" && diagnostics.speed_mapping.codec_key == "aom:cpu-used" &&
                diagnostics.speed_mapping.codec_value == 7, "AOM diagnostics are inconsistent");
        }
      }
    }
  }
  awj::AvifEncoderSelectionRequest request{.pixel_count = 1024, .width = 32, .height = 32};
  auto selected = awj::select_avif_encoder_from_capabilities(request, capabilities);
  check(selected && selected->applied_bit_depth == 10 && selected->speed == awj::encoding_defaults::default_aom_cpu_used,
        "Automatic AOM defaults changed");
  check(!awj::select_avif_encoder_from_capabilities(request, awj::avif_encoder_capabilities_for_build(false)),
        "Unavailable AOM silently selected another encoder");
  request.requested_bit_depth = 16;
  check(!awj::select_avif_encoder_from_capabilities(request, capabilities), "Explicit unsupported bit depth accepted");
  request.requested_bit_depth_explicit = false;
  selected = awj::select_avif_encoder_from_capabilities(request, capabilities);
  check(selected && selected->applied_bit_depth == 12 && !selected->bit_depth_reason.empty(),
        "Inherited 16-bit source was not bounded to AOM 12-bit with explanation");
  request.requested_bit_depth.reset();
  for (const auto dimensions : {awj::make_image_dimensions(65536, 1), awj::make_image_dimensions(32768, 32768)}) {
    request.width = dimensions.width;
    request.height = dimensions.height;
    request.pixel_count = dimensions.pixel_count;
    check(awj::select_avif_encoder_from_capabilities(request, capabilities).has_value(), "Exact single-image limit rejected");
    check(awj::classify_large_image(dimensions, true).klass != awj::LargeImageClass::large_mode_required,
          "Exact single-image limit incorrectly routed to grid");
  }
  for (const auto dimensions : {awj::make_image_dimensions(65537, 1), awj::make_image_dimensions(32768, 32769)}) {
    request.width = dimensions.width;
    request.height = dimensions.height;
    request.pixel_count = dimensions.pixel_count;
    check(!awj::select_avif_encoder_from_capabilities(request, capabilities), "Oversized single-image request accepted");
    const auto decision = awj::classify_large_image(dimensions, true);
    check(decision.klass == awj::LargeImageClass::large_mode_required && decision.available_grid,
          "Oversized image did not route to AOM Grid");
  }
  const auto grid = awj::plan_grid({.width = 48017, .height = 32003});
  check(grid && grid->uses_padding && grid->clamped_to_original_size &&
        grid->padded_width >= 48017 && grid->padded_height >= 32003, "Odd grid dimensions lost edge cells");
  check(!awj::classify_large_image(awj::make_image_dimensions(70000, 1), false).available_grid,
        "Unavailable grid advertised");
  std::puts("AOM selection, depth/chroma/alpha, diagnostics and automatic Grid boundaries passed.");
  return 0;
} catch (const std::exception& error) {
  std::fprintf(stderr, "%s\n", error.what());
  return 1;
}
