#pragma once

#include <cstddef>
#include <expected>
#include <memory>
#include <string>
#include <vector>

#include <lcms2.h>

namespace awj::test {

// Public Display P3/D65 coordinates and the standard sRGB EOTF. This fixture
// does not borrow production color resolvers or any external/private image.
inline std::expected<std::vector<std::byte>, std::string> display_p3_srgb_icc() {
  const cmsCIExyY white{0.3127, 0.3290, 1.0};
  const cmsCIExyYTRIPLE primaries{{0.680, 0.320, 1.0}, {0.265, 0.690, 1.0}, {0.150, 0.060, 1.0}};
  const cmsFloat64Number srgb[]{2.4, 1.0 / 1.055, 0.055 / 1.055, 1.0 / 12.92, 0.04045};
  std::unique_ptr<cmsToneCurve, decltype(&cmsFreeToneCurve)> curve{
      cmsBuildParametricToneCurve(nullptr, 4, srgb), &cmsFreeToneCurve};
  if (!curve) return std::unexpected{"Cannot create Display P3/sRGB fixture curve."};
  cmsToneCurve* curves[]{curve.get(), curve.get(), curve.get()};
  std::unique_ptr<void, decltype(&cmsCloseProfile)> profile{
      cmsCreateRGBProfile(&white, &primaries, curves), &cmsCloseProfile};
  if (!profile) return std::unexpected{"Cannot create Display P3/sRGB fixture profile."};
  cmsSetProfileVersion(profile.get(), 4.3);
  cmsUInt32Number size{};
  if (!cmsSaveProfileToMem(profile.get(), nullptr, &size) || size < 132)
    return std::unexpected{"Cannot size Display P3/sRGB fixture ICC."};
  std::vector<std::byte> bytes(size);
  if (!cmsSaveProfileToMem(profile.get(), bytes.data(), &size))
    return std::unexpected{"Cannot serialize Display P3/sRGB fixture ICC."};
  bytes.resize(size);
  return bytes;
}

}  // namespace awj::test
