#pragma once

#include <limits>

namespace awj::library_memory_policy {

// libheif 1.23.7 heif_security.h: zero disables a field. Change only the
// instance's allocation/pixel-memory quotas. Tile/item/component/frame counts,
// box structure, and uncompressed per-pixel stride validation stay intact.
template <class HeifLimits>
constexpr void apply_heif(HeifLimits& limits, bool unrestricted_memory) noexcept {
  if (!unrestricted_memory) return;
  limits.max_image_size_pixels = 0; // documented as a memory-allocation guard
  limits.max_color_profile_size = 0;
  limits.max_memory_block_size = 0;
  if (limits.version >= 2) limits.max_total_memory = 0;
}

// LibRaw 0.22.3 compares allocation bytes to INT64(max_raw_memory_mb)*MiB;
// zero is NOT unlimited. Use the parameter's representable maximum, not a new
// AWJ byte budget. Its 65535x65535 image-format bounds remain in unpack.cpp and
// constrain representable images well below this unsigned-MiB API ceiling.
template <class RawParams>
constexpr void apply_libraw(RawParams& params, bool unrestricted_memory) noexcept {
  if (unrestricted_memory)
    params.max_raw_memory_mb = std::numeric_limits<decltype(params.max_raw_memory_mb)>::max();
}

}  // namespace awj::library_memory_policy
