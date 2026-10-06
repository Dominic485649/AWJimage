module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <numeric>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

export module awj.animation;

import awj.image;

export namespace awj {

struct FrameDuration {
  std::uint32_t numerator{};
  std::uint32_t denominator{100};
};

struct AnimationInfo {
  std::size_t width{};
  std::size_t height{};
  std::vector<FrameDuration> durations{};
  // Repeat count after the first playback: -1 means infinite; 0 means play once.
  int repetitions{};
  bool zero_delay_adjusted{};
  std::string decoder_id{};
};

// Sequential full-canvas frames. next() invalidates the previous borrowed image.
// Only the compressed source, canvas, current rectangle and PREVIOUS snapshot are retained.
class AnimationReader {
 public:
  virtual ~AnimationReader() = default;
  virtual const AnimationInfo& info() const noexcept = 0;
  virtual std::expected<bool, std::string> next(std::stop_token stop = {}) = 0;
  virtual const ImageBuffer& frame() const noexcept = 0;
  virtual std::uint64_t memory_usage_bytes() const noexcept = 0;
};

std::uint64_t animation_timescale(std::span<const FrameDuration> durations) noexcept {
  std::uint64_t cap = 1000000;
  // AVIF's stts sample_delta is uint32_t even though AddImage accepts uint64_t.
  // Leave one tick for the difference of two rounded cumulative timestamps.
  for (auto duration : durations) {
    if (!duration.numerator || !duration.denominator) return cap;
    const auto safe_scale = (std::uint64_t{std::numeric_limits<std::uint32_t>::max()} - 1) *
        duration.denominator / duration.numerator;
    cap = std::min(cap, std::max(std::uint64_t{1}, safe_scale));
  }
  std::uint64_t timescale = 1;
  for (auto duration : durations) {
    if (!duration.denominator) return cap;
    const auto denominator = duration.denominator / std::gcd(duration.numerator, duration.denominator);
    const auto factor = denominator / std::gcd(timescale, static_cast<std::uint64_t>(denominator));
    if (factor > cap / timescale) return cap;
    timescale *= factor;
  }
  return timescale;
}

} // namespace awj
