module;

#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <fstream>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>
#include <ultrahdr_api.h>
#include <avif/avif.h>
#include <avif/internal.h>
#include <lcms2.h>
#include <libplacebo/colorspace.h>
#include "uhdr_thread_adapter.h"
#include "avif_gain_map_thread_adapter.h"

export module awj.gain_map;

import awj.codec;
import awj.decoder_common;
import awj.resource_planner;
import awj.encoding_defaults;
import awj.hdr_tonemap;
import awj.image;
import awj.large_image_plan;
import awj.pixel_conversion;

export namespace awj {

using IsoGainMapPtr = std::unique_ptr<avifGainMap, decltype(&avifGainMapDestroy)>;

std::expected<ImageDecodeResult, std::string> render_iso_gain_map(
    ImageBuffer base, const ImageBuffer& map, avifGainMap& metadata,
    GainMapDecodeMode mode = GainMapDecodeMode::hdr_only, std::stop_token stop = {},
    const ResourcePlan& resources = {}, EncodeTimingDiagnostics* timing = nullptr);

std::expected<IsoGainMapPtr, std::string> compute_iso_gain_map(
    const ImageBuffer& main, const GainMapAlternative& alternative,
    std::uint32_t map_width = 0, std::uint32_t map_height = 0,
    int map_depth = 8, std::stop_token stop = {}, const ResourcePlan& resources = {},
    EncodeTimingDiagnostics* timing = nullptr);

namespace gain_map_detail {

// 为已知 CICP 的 SDR 呈现生成必要 ICC，不改变像素，也不扩大输入 ICC 的放行范围。
std::expected<std::vector<std::byte>, std::string> sdr_presentation_icc(
    const ImageSourceInfo& info, std::stop_token stop = {}) {
  if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
  if (!info.color_primaries || !info.transfer_characteristics || info.has_hdr_metadata)
    return std::unexpected{"SDR 呈现缺少可解释色彩信息。"};
  const auto* raw = hdr::hdr_detail::primaries_from_cicp(*info.color_primaries);
  const int transfer = *info.transfer_characteristics;
  if (!raw || !(transfer == 1 || (transfer >= 4 && transfer <= 15) || transfer == 17))
    return std::unexpected{"SDR 呈现使用暂不支持的原色或传递函数。"};
  const auto to_linear = avifTransferCharacteristicsGetGammaToLinearFunction(static_cast<avifTransferCharacteristics>(transfer));
  std::array<cmsFloat32Number, 4097> samples{};
  for (std::size_t i = 0; i < samples.size(); ++i) {
    if (i % 128 == 0 && stop.stop_requested()) return std::unexpected{"任务已取消。"};
    const auto value = to_linear(static_cast<float>(i) / (samples.size() - 1));
    if (!std::isfinite(value) || value < 0 || value > 1.00001F)
      return std::unexpected{"SDR 传递函数不能表示为 SDR ICC。"};
    samples[i] = std::clamp(value, 0.0F, 1.0F);
  }
  std::unique_ptr<cmsToneCurve, decltype(&cmsFreeToneCurve)> curve{
      cmsBuildTabulatedToneCurveFloat(nullptr, static_cast<cmsUInt32Number>(samples.size()), samples.data()), &cmsFreeToneCurve};
  if (!curve) return std::unexpected{"无法创建 SDR ICC 传递曲线。"};
  const cmsCIExyY white{raw->white.x, raw->white.y, 1.0};
  const cmsCIExyYTRIPLE primaries{{raw->red.x, raw->red.y, 1.0}, {raw->green.x, raw->green.y, 1.0}, {raw->blue.x, raw->blue.y, 1.0}};
  cmsToneCurve* curves[]{curve.get(), curve.get(), curve.get()};
  std::unique_ptr<void, decltype(&cmsCloseProfile)> profile{cmsCreateRGBProfile(&white, &primaries, curves), &cmsCloseProfile};
  if (!profile) return std::unexpected{"无法创建 SDR ICC 色彩描述。"};
  cmsSetProfileVersion(profile.get(), 4.3);
  cmsUInt32Number size{};
  if (!cmsSaveProfileToMem(profile.get(), nullptr, &size) || !size || size > encoding_defaults::codec_metadata_max_bytes)
    return std::unexpected{"SDR ICC 输出大小无效。"};
  std::vector<std::byte> bytes(size);
  if (!cmsSaveProfileToMem(profile.get(), bytes.data(), &size) || size != bytes.size())
    return std::unexpected{"SDR ICC 序列化失败。"};
  return bytes;
}

bool supported_iso_transfer(int transfer) noexcept {
  return transfer == 1 || (transfer >= 4 && transfer <= 18);
}

struct IsoColorSpace {
  int primaries{};
  int transfer{};
  bool verified_icc{};
};

std::uint32_t icc_u32(std::span<const std::byte> bytes, std::size_t at) noexcept {
  return (std::to_integer<std::uint32_t>(bytes[at]) << 24) |
      (std::to_integer<std::uint32_t>(bytes[at + 1]) << 16) |
      (std::to_integer<std::uint32_t>(bytes[at + 2]) << 8) |
      std::to_integer<std::uint32_t>(bytes[at + 3]);
}

double icc_fixed(std::span<const std::byte> bytes, std::size_t at) noexcept {
  const auto value = icc_u32(bytes, at);
  const auto signed_value = value < 0x80000000u ? static_cast<std::int64_t>(value)
      : static_cast<std::int64_t>(value) - 0x100000000ll;
  return static_cast<double>(signed_value) / 65536.0;
}

// 只接受可证明等价的矩阵/TRC；描述名、解码器默认值和另一端的色彩标签都不是证据。
// 容差限于 ICC 16.16 序列化误差，不能据此宣称双呈现或重新编码逐样本无损。
std::expected<IsoColorSpace, std::string> icc_iso_color(
    std::span<const std::byte> bytes, std::stop_token stop) {
  constexpr auto error = "Gain Map: ISO 21496-1: ICC 损坏或超出受支持的 RGB 矩阵/TRC 色彩空间。";
  if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
  if (bytes.size() < 132 || bytes.size() > encoding_defaults::codec_metadata_max_bytes ||
      bytes.size() > UINT32_MAX || icc_u32(bytes, 0) != bytes.size() ||
      icc_u32(bytes, 36) != 0x61637370u ||
      (std::to_integer<unsigned>(bytes[8]) != 2 && std::to_integer<unsigned>(bytes[8]) != 4))
    return std::unexpected{error};
  const auto device_class = icc_u32(bytes, 12);
  if ((device_class != cmsSigDisplayClass && device_class != cmsSigInputClass) ||
      icc_u32(bytes, 16) != cmsSigRgbData || icc_u32(bytes, 20) != cmsSigXYZData || icc_u32(bytes, 64) > 3)
    return std::unexpected{error};
  const auto close = [](double a, double b, double tolerance = 4.0 / 65536.0) {
    return std::isfinite(a) && std::isfinite(b) && std::abs(a - b) <= tolerance;
  };
  const auto* d50 = cmsD50_XYZ();
  if (!close(icc_fixed(bytes, 68), d50->X) || !close(icc_fixed(bytes, 72), d50->Y) ||
      !close(icc_fixed(bytes, 76), d50->Z)) return std::unexpected{error};
  struct Tag { std::uint32_t signature{}, offset{}, size{}; };
  std::array<Tag, 128> tags{};
  const auto tag_count = icc_u32(bytes, 128);
  if (!tag_count || tag_count > tags.size() || tag_count > (bytes.size() - 132) / 12)
    return std::unexpected{error};
  const auto table_end = 132u + tag_count * 12u;
  for (std::size_t i = 0; i < tag_count; ++i) {
    auto& tag = tags[i];
    tag = {icc_u32(bytes, 132 + i * 12), icc_u32(bytes, 136 + i * 12), icc_u32(bytes, 140 + i * 12)};
    if (tag.offset < table_end || tag.offset % 4 || tag.offset > bytes.size() ||
        tag.size < 8 || tag.size > bytes.size() - tag.offset || icc_u32(bytes, tag.offset + 4) != 0)
      return std::unexpected{error};
    for (std::size_t j = 0; j < i; ++j) {
      const auto& other = tags[j];
      if (tag.signature == other.signature ||
          ((tag.offset != other.offset || tag.size != other.size) &&
           tag.offset < other.offset + other.size && other.offset < tag.offset + tag.size))
        return std::unexpected{error};
    }
    const auto type = icc_u32(bytes, tag.offset);
    switch (tag.signature) {
      case cmsSigRedColorantTag: case cmsSigGreenColorantTag: case cmsSigBlueColorantTag:
      case cmsSigMediaWhitePointTag: case cmsSigMediaBlackPointTag:
        if (type != cmsSigXYZType || tag.size != 20) return std::unexpected{error};
        break;
      case cmsSigChromaticAdaptationTag:
        if (type != cmsSigS15Fixed16ArrayType || tag.size != 44) return std::unexpected{error};
        break;
      case cmsSigChromaticityTag:
        if (type != cmsSigChromaticityType || tag.size != 36 || icc_u32(bytes, tag.offset + 8) != 0x00030000u)
          return std::unexpected{error};
        break;
      case cmsSigRedTRCTag: case cmsSigGreenTRCTag: case cmsSigBlueTRCTag:
        // 多点曲线、mAB/mBA 和未知 LUT 不通过抽样猜测为标准传递函数。
        if (type == cmsSigCurveType) {
          if (tag.size < 12 || icc_u32(bytes, tag.offset + 8) > 1 ||
              tag.size != 12u + 2u * icc_u32(bytes, tag.offset + 8)) return std::unexpected{error};
        } else if (type == cmsSigParametricCurveType) {
          if (tag.size < 12) return std::unexpected{error};
          const auto function = icc_u32(bytes, tag.offset + 8);
          if (!((function == 0 && tag.size == 16) || (function == 0x00030000u && tag.size == 32) ||
                (function == 0x00040000u && tag.size == 40))) return std::unexpected{error};
        } else return std::unexpected{error};
        break;
      case cmsSigcicpTag:
        if (type != cmsSigcicpType || tag.size != 12) return std::unexpected{error};
        break;
      case cmsSigProfileDescriptionTag: case cmsSigCopyrightTag:
      case cmsSigDeviceMfgDescTag: case cmsSigDeviceModelDescTag:
        // 文本只允许作为无关描述保留，绝不读取其内容来决定原色或曲线。
        if (type != cmsSigTextType && type != cmsSigTextDescriptionType && type != cmsSigMultiLocalizedUnicodeType)
          return std::unexpected{error};
        break;
      default:
        // 尤其拒绝 A2B/B2A/D2B/B2D、vcgt、MHC2 等另有运算语义的标签。
        return std::unexpected{"Gain Map: ISO 21496-1: ICC 含不支持的处理标签或 LUT，不能按矩阵/TRC 猜测。"};
    }
  }
  const auto tag_bytes = [&](cmsTagSignature signature) -> std::span<const std::byte> {
    for (std::size_t i = 0; i < tag_count; ++i)
      if (tags[i].signature == signature) return bytes.subspan(tags[i].offset, tags[i].size);
    return {};
  };
  // 每次调用独立的 LCMS context/profile；不写全局错误回调，不缓存借用的 tag 指针。
  std::unique_ptr<std::remove_pointer_t<cmsContext>, decltype(&cmsDeleteContext)> context{
      cmsCreateContext(nullptr, nullptr), &cmsDeleteContext};
  if (!context) return std::unexpected{"Gain Map: ISO 21496-1: 无法创建 ICC 校验 context。"};
  using Profile = std::unique_ptr<void, decltype(&cmsCloseProfile)>;
  Profile profile{cmsOpenProfileFromMemTHR(context.get(), bytes.data(), static_cast<cmsUInt32Number>(bytes.size())), &cmsCloseProfile};
  if (!profile || !cmsIsMatrixShaper(profile.get())) return std::unexpected{error};
  const auto* white = static_cast<const cmsCIEXYZ*>(cmsReadTag(profile.get(), cmsSigMediaWhitePointTag));
  if (!white) return std::unexpected{error};
  const auto xyz_close = [&](const cmsCIEXYZ& a, const cmsCIEXYZ& b) {
    return close(a.X, b.X) && close(a.Y, b.Y) && close(a.Z, b.Z);
  };
  if (cmsIsTag(profile.get(), cmsSigMediaBlackPointTag)) {
    const auto* black = static_cast<const cmsCIEXYZ*>(cmsReadTag(profile.get(), cmsSigMediaBlackPointTag));
    if (!black || !xyz_close(*black, cmsCIEXYZ{})) return std::unexpected{error};
  }
  int primaries{};
  const auto chad = tag_bytes(cmsSigChromaticAdaptationTag);
  // ICC PCS 固定 D50 与常用 CIE 1931 二度 D50 XYZ 并非同一组舍入常数。
  // 只承认这两种标准白点约定，不把任意 chad 或更宽容差当成 CICP 等价。
  const cmsCIEXYZ cie_d50{0.96422, 1.0, 0.82521};
  for (int candidate : {1, 9, 12}) {
    const auto* raw = hdr::hdr_detail::primaries_from_cicp(candidate);
    if (!raw) continue;
    const cmsCIExyY native_white{raw->white.x, raw->white.y, 1.0};
    cmsCIEXYZ native_xyz{};
    cmsxyY2XYZ(&native_xyz, &native_white);
    const cmsCIExyYTRIPLE colors{{raw->red.x, raw->red.y, 1.0},
        {raw->green.x, raw->green.y, 1.0}, {raw->blue.x, raw->blue.y, 1.0}};
    Profile reference{cmsCreateRGBProfileTHR(context.get(), &native_white, &colors, nullptr), &cmsCloseProfile};
    if (!reference) return std::unexpected{"Gain Map: ISO 21496-1: 无法创建 ICC 原色参考。"};
    const bool v2_display = std::to_integer<unsigned>(bytes[8]) == 2 && device_class == cmsSigDisplayClass;
    std::array<double, 9> adaptation{};
    // V2 显示 profile 的原生 wtpt 可隐含 Bradford；D50 白点但无 chad 不能证明原生白点。
    if (chad.empty()) {
      if (!v2_display || !xyz_close(*white, native_xyz)) continue;
      std::array<std::byte, 44> implicit_chad{};
      if (cmsReadRawTag(reference.get(), cmsSigChromaticAdaptationTag, implicit_chad.data(),
          static_cast<cmsUInt32Number>(implicit_chad.size())) != implicit_chad.size()) return std::unexpected{error};
      for (std::size_t i = 0; i < adaptation.size(); ++i) adaptation[i] = icc_fixed(implicit_chad, 8 + i * 4);
    } else {
      if (!xyz_close(*white, *d50) && !xyz_close(*white, cie_d50) &&
          !(v2_display && xyz_close(*white, native_xyz))) continue;
      for (std::size_t i = 0; i < adaptation.size(); ++i) adaptation[i] = icc_fixed(chad, 8 + i * 4);
      bool known_adaptation = false;
      for (const auto& pcs_white : {*d50, cie_d50}) {
        bool matches = true;
        const std::array<cmsCIEXYZ, 3> axes{{{1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0}}};
        for (std::size_t c = 0; c < axes.size(); ++c) {
          cmsCIEXYZ expected{};
          if (!cmsAdaptToIlluminant(&expected, &native_xyz, &pcs_white, &axes[c])) return std::unexpected{error};
          matches &= close(adaptation[c], expected.X) && close(adaptation[3 + c], expected.Y) &&
              close(adaptation[6 + c], expected.Z);
        }
        known_adaptation |= matches;
      }
      if (!known_adaptation) continue;
    }
    bool matches = true;
    for (auto signature : {cmsSigRedColorantTag, cmsSigGreenColorantTag, cmsSigBlueColorantTag}) {
      const auto* actual = static_cast<const cmsCIEXYZ*>(cmsReadTag(profile.get(), signature));
      const auto* reference_pcs = static_cast<const cmsCIEXYZ*>(cmsReadTag(reference.get(), signature));
      if (!actual || !reference_pcs) { matches = false; break; }
      cmsCIEXYZ native{};
      if (!cmsAdaptToIlluminant(&native, d50, &native_xyz, reference_pcs)) return std::unexpected{error};
      // 验证 M_profile = CHAD_profile × M_CICP_native；不对不可信矩阵求逆，
      // 也不要求使用不同 D50 常数生成的 PCS 列逐项相等。工作像素仍完全不变。
      const cmsCIEXYZ expected{
          adaptation[0] * native.X + adaptation[1] * native.Y + adaptation[2] * native.Z,
          adaptation[3] * native.X + adaptation[4] * native.Y + adaptation[5] * native.Z,
          adaptation[6] * native.X + adaptation[7] * native.Y + adaptation[8] * native.Z};
      matches &= xyz_close(*actual, expected);
    }
    const auto chromaticity = tag_bytes(cmsSigChromaticityTag);
    if (!chromaticity.empty()) {
      const std::array<double, 6> xy{colors.Red.x, colors.Red.y, colors.Green.x, colors.Green.y, colors.Blue.x, colors.Blue.y};
      for (std::size_t i = 0; i < xy.size(); ++i)
        matches &= close(static_cast<double>(icc_u32(chromaticity, 12 + i * 4)) / 65536.0, xy[i]);
    }
    if (matches) { primaries = candidate; break; }
  }
  if (!primaries) return std::unexpected{"Gain Map: ISO 21496-1: ICC 原色、白点或适应矩阵不能验证为受支持的 CICP。"};
  const std::array<cmsTagSignature, 3> trc_tags{cmsSigRedTRCTag, cmsSigGreenTRCTag, cmsSigBlueTRCTag};
  std::array<const cmsToneCurve*, 3> curves{};
  for (std::size_t c = 0; c < curves.size(); ++c) {
    curves[c] = static_cast<const cmsToneCurve*>(cmsReadTag(profile.get(), trc_tags[c]));
    if (!curves[c] || cmsIsToneCurveMultisegment(curves[c])) return std::unexpected{error};
  }
  int transfer{};
  for (int candidate : {13, 8, 4, 5}) {
    bool matches = true;
    for (const auto* curve : curves) {
      const auto* segment = cmsGetToneCurveSegment(0, curve);
      if (!segment) { matches = false; break; }
      if (candidate == 13) {
        if (segment->Type != 4 && segment->Type != 5) { matches = false; break; }
        constexpr std::array<double, 7> srgb{2.4, 1.0 / 1.055, 0.055 / 1.055, 1.0 / 12.92, 0.04045, 0.0, 0.0};
        for (std::size_t p = 0; p < (segment->Type == 4 ? 5u : 7u); ++p)
          matches &= close(segment->Params[p], srgb[p], 1.1 / 65536.0);
      } else {
        const double gamma = candidate == 8 ? 1.0 : candidate == 4 ? 2.2 : 2.8;
        matches &= segment->Type == 1 && close(segment->Params[0], gamma, 1.1 / 65536.0);
      }
    }
    if (!matches) continue;
    const auto to_linear = avifTransferCharacteristicsGetGammaToLinearFunction(static_cast<avifTransferCharacteristics>(candidate));
    if (!to_linear) return std::unexpected{error};
    // 已限制解析函数形状，再核对全部 16-bit 输入码值；不以少量采样漏过窄区间偏差。
    for (std::uint32_t sample = 0; sample <= 65535 && matches; ++sample) {
      if (sample % 1024 == 0 && stop.stop_requested()) return std::unexpected{"任务已取消。"};
      const float encoded = static_cast<float>(sample) / 65535.0F;
      const float expected = to_linear(encoded);
      for (const auto* curve : curves)
        matches &= close(cmsEvalToneCurveFloat(curve, encoded), expected, 2.0 / 65535.0);
    }
    if (matches) { transfer = candidate; break; }
  }
  if (!transfer) return std::unexpected{"Gain Map: ISO 21496-1: ICC TRC 不能验证为受支持的标准传递函数。"};
  const auto cicp = tag_bytes(cmsSigcicpTag);
  if (!cicp.empty() && (std::to_integer<int>(cicp[8]) != primaries || std::to_integer<int>(cicp[9]) != transfer ||
      cicp[10] != std::byte{0} || cicp[11] != std::byte{1}))
    return std::unexpected{"Gain Map: ISO 21496-1: ICC cicp 与实际矩阵/TRC 冲突。"};
  return IsoColorSpace{primaries, transfer, true};
}

bool explicit_iso_tag(std::optional<int> tag) noexcept {
  return tag && *tag != 0 && *tag != 2;
}

std::expected<IsoColorSpace, std::string> resolve_iso_color(
    const ImageBuffer& source, std::stop_token stop) {
  if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
  const MetadataBlock* icc = nullptr;
  std::size_t metadata_bytes{};
  if (source.metadata.size() > encoding_defaults::jpeg_max_saved_metadata_marker_count)
    return std::unexpected{"Gain Map: ISO 21496-1: metadata 数量超过上限。"};
  for (const auto& block : source.metadata) {
    if (block.bytes.size() > encoding_defaults::codec_metadata_max_bytes - metadata_bytes)
      return std::unexpected{"Gain Map: ISO 21496-1: metadata 大小超过上限。"};
    metadata_bytes += block.bytes.size();
    if (block.kind != MetadataKind::icc) continue;
    if (icc) return std::unexpected{"Gain Map: ISO 21496-1: 多个 ICC 不能确定唯一运算空间。"};
    icc = &block;
  }
  const auto cp = source.source_info ? source.source_info->color_primaries : std::optional<int>{};
  const auto tc = source.source_info ? source.source_info->transfer_characteristics : std::optional<int>{};
  if (!icc) {
    if (!cp || !tc || !hdr::hdr_detail::primaries_from_cicp(*cp) || !supported_iso_transfer(*tc))
      return std::unexpected{"Gain Map: ISO 21496-1: 输入缺少明确或受支持的编码色彩空间。"};
    return IsoColorSpace{*cp, *tc, false};
  }
  if (source.sample_representation != SampleRepresentation::unorm)
    return std::unexpected{"Gain Map: ISO 21496-1: ICC 仅支持已定义 [0,1] 编码域的整数采样。"};
  auto color = icc_iso_color(icc->bytes, stop);
  if (!color) return color;
  // NCLX 的矩阵/范围描述编码 YUV；这里只校验与 ICC 共用的原色和传递函数。
  if ((explicit_iso_tag(cp) && *cp != color->primaries) || (explicit_iso_tag(tc) && *tc != color->transfer))
    return std::unexpected{"Gain Map: ISO 21496-1: ICC 与显式 CICP 原色或传递函数冲突。"};
  return color;
}

std::expected<void, std::string> normalize_iso_alternate(avifGainMap& local, std::stop_token stop) {
  if (!local.altICC.size) return {};
  if (!local.altICC.data || local.altICC.size > encoding_defaults::codec_metadata_max_bytes)
    return std::unexpected{"Gain Map: ISO 21496-1: alternate ICC 无效或超过大小上限。"};
  auto color = icc_iso_color({reinterpret_cast<const std::byte*>(local.altICC.data), local.altICC.size}, stop);
  if (!color) return std::unexpected{color.error()};
  if ((explicit_iso_tag(static_cast<int>(local.altColorPrimaries)) && local.altColorPrimaries != color->primaries) ||
      (explicit_iso_tag(static_cast<int>(local.altTransferCharacteristics)) && local.altTransferCharacteristics != color->transfer))
    return std::unexpected{"Gain Map: ISO 21496-1: alternate ICC 与显式 CICP 冲突。"};
  local.altColorPrimaries = static_cast<avifColorPrimaries>(color->primaries);
  local.altTransferCharacteristics = static_cast<avifTransferCharacteristics>(color->transfer);
  // local 是只读元数据的浅复制，不拥有原 altICC；这里不能调用释放函数。
  local.altICC = {};
  return {};
}

void normalize_iso_base(ImageBuffer& image, const IsoColorSpace& color) {
  if (!color.verified_icc) return;
  if (!image.source_info) image.source_info = ImageSourceInfo{.pixel_format = image.pixel_format, .bit_depth = image.bit_depth};
  auto& info = *image.source_info;
  info.color_primaries = color.primaries;
  info.transfer_characteristics = color.transfer;
  // ICC 只证明 RGB 原色和 transfer 等价，不能改写仍在使用的 YUV 矩阵／范围。
  if (!is_yuv_format(image.pixel_format)) {
    info.matrix_coefficients = 0;
    info.color_range = 1;
  }
  info.color_metadata_source = "source-icc-verified-cicp";
  // 像素不变，只替换已经验证等价的解释；strip 之后 CICP 仍解释工作像素。
  std::erase_if(image.metadata, [](const MetadataBlock& block) { return block.kind == MetadataKind::icc; });
}

std::expected<void, std::string> transform_rgba(ImageBuffer& image, unsigned ccw_quarters, int mirror = -1) {
  if (auto valid = hdr::hdr_detail::validate_rgba(image, "ISO 21496-1 orientation"); !valid) return valid;
  ccw_quarters %= 4;
  if (!ccw_quarters && mirror < 0) return {};
  const auto width = ccw_quarters & 1 ? image.height : image.width;
  const auto height = ccw_quarters & 1 ? image.width : image.height;
  const auto sample_bytes = image.bit_depth == 16 ? 2u : 1u;
  auto bytes = hdr::hdr_detail::make_rgba_bytes(width, height, sample_bytes, "ISO 21496-1 orientation");
  if (!bytes) return std::unexpected{bytes.error()};
  const auto pixel_bytes = sample_bytes * 4;
  for (std::size_t y = 0; y < image.height; ++y) for (std::size_t x = 0; x < image.width; ++x) {
    std::size_t dx = x, dy = y;
    if (ccw_quarters == 1) { dx = y; dy = image.width - 1 - x; }
    if (ccw_quarters == 2) { dx = image.width - 1 - x; dy = image.height - 1 - y; }
    if (ccw_quarters == 3) { dx = image.height - 1 - y; dy = x; }
    if (mirror == 0) dy = height - 1 - dy;
    if (mirror == 1) dx = width - 1 - dx;
    std::memcpy(bytes->data() + (dy * width + dx) * pixel_bytes,
        image.planes.front().bytes.data() + y * image.planes.front().stride + x * pixel_bytes, pixel_bytes);
  }
  image.width = width; image.height = height;
  image.planes.front() = {std::move(*bytes), width * pixel_bytes};
  return {};
}

void normalize_exif_orientation(ImageBuffer& image) noexcept {
  for (auto& block : image.metadata) if (block.kind == MetadataKind::exif) {
    std::size_t offset{};
    if (avifGetExifOrientationOffset(reinterpret_cast<const std::uint8_t*>(block.bytes.data()),
        block.bytes.size(), &offset) == AVIF_RESULT_OK && offset < block.bytes.size()) block.bytes[offset] = std::byte{1};
  }
}

std::expected<void, std::string> apply_exif_orientation(ImageBuffer& image) {
  for (auto& block : image.metadata) if (block.kind == MetadataKind::exif) {
    std::size_t offset{};
    if (avifGetExifOrientationOffset(reinterpret_cast<const std::uint8_t*>(block.bytes.data()),
        block.bytes.size(), &offset) != AVIF_RESULT_OK || offset >= block.bytes.size()) continue;
    const auto orientation = std::to_integer<unsigned>(block.bytes[offset]);
    unsigned rotation = 0; int mirror = -1;
    switch (orientation) {
      case 1: break;
      case 2: mirror = 1; break;
      case 3: rotation = 2; break;
      case 4: mirror = 0; break;
      case 5: rotation = 1; mirror = 0; break;
      case 6: rotation = 3; break;
      case 7: rotation = 1; mirror = 1; break;
      case 8: rotation = 1; break;
      default: return std::unexpected{"Gain Map: ISO 21496-1: Exif orientation 无效。"};
    }
    auto transformed = transform_rgba(image, rotation, mirror);
    if (!transformed) return transformed;
    normalize_exif_orientation(image);
    break;
  }
  return {};
}

std::expected<void, std::string> crop_rgba(ImageBuffer& image, std::size_t x, std::size_t y,
    std::size_t width, std::size_t height) {
  if (!width || !height || x > image.width || y > image.height ||
      width > image.width - x || height > image.height - y) return std::unexpected{"Gain Map: ISO 21496-1: crop 越界。"};
  const auto sample_bytes = image.bit_depth == 16 ? 2u : 1u;
  auto bytes = hdr::hdr_detail::make_rgba_bytes(width, height, sample_bytes, "ISO 21496-1 crop");
  if (!bytes) return std::unexpected{bytes.error()};
  const auto stride = width * 4 * sample_bytes;
  for (std::size_t row = 0; row < height; ++row)
    std::memcpy(bytes->data() + row * stride,
        image.planes.front().bytes.data() + (y + row) * image.planes.front().stride + x * 4 * sample_bytes, stride);
  image.width = width; image.height = height; image.planes.front() = {std::move(*bytes), stride};
  return {};
}

std::string uhdr_error(const uhdr_error_info_t& error) {
  return std::string{"Gain Map: ISO 21496-1: "} +
      (error.has_detail ? error.detail : "unsupported or invalid image");
}

// This policy consumes libavif's authoritative parse result/diagnostic only;
// it does not inspect container bytes. Unrelated diagnostics are not HDR.
bool avif_iso_gain_map_failure(avifResult result, const avifDecoder* decoder) noexcept {
  return result == AVIF_RESULT_INVALID_TONE_MAPPED_IMAGE || result == AVIF_RESULT_DECODE_GAIN_MAP_FAILED ||
      (decoder && ((result != AVIF_RESULT_OK && decoder->image && decoder->image->gainMap) ||
       std::string_view{decoder->diag.error}.starts_with("Box[tmap]")));
}

std::expected<void, std::string> validate_avif_iso_gain_map_parse(avifResult result, const avifDecoder& decoder) {
  // Pinned libavif ignores unsupported tmap versions, returning OK while
  // leaving this diagnostic. Inspect it before NextImage() clears diag.
  if (avif_iso_gain_map_failure(result, &decoder))
    return std::unexpected{std::string{"Gain Map: ISO 21496-1 AVIF: "} +
        (decoder.diag.error[0] ? decoder.diag.error : avifResultToString(result))};
  if (result == AVIF_RESULT_OK && decoder.image && decoder.image->gainMap) {
    avifDiagnostics diagnostics{};
    if (avifGainMapValidateMetadata(decoder.image->gainMap, &diagnostics) != AVIF_RESULT_OK)
      return std::unexpected{std::string{"Gain Map: ISO 21496-1 AVIF metadata: "} + diagnostics.error};
  }
  return {};
}

inline constexpr std::string_view legacy_gain_map_error =
    "Gain Map: ISO 21496-1 only; legacy Adobe/Google/Apple gain maps are unsupported.";

bool xml_space(char c) noexcept { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// Rejection signatures only, not an XMP metadata parser. An unused namespace
// declaration (or an xmlns prefix named after a property) is not HDR content.
std::size_t xmp_property_value(std::string_view text, std::string_view name, std::size_t start = 0) {
  for (auto pos = text.find(name, start); pos != text.npos; pos = text.find(name, pos + name.size())) {
    if (!pos || (text[pos - 1] != ':' && text[pos - 1] != '<')) continue;
    if (text[pos - 1] == ':') {
      auto prefix = pos - 1;
      while (prefix && text[prefix - 1] != '<' && !xml_space(text[prefix - 1])) --prefix;
      if (text.substr(prefix, pos - prefix - 1) == "xmlns") continue;
    }
    auto value = pos + name.size();
    while (value < text.size() && xml_space(text[value])) ++value;
    if (value < text.size() && (text[value] == '=' || text[value] == '>')) return value;
  }
  return text.npos;
}

bool xmp_has_legacy_gain_map(std::string_view text) {
  if (text.find("http://ns.adobe.com/hdr-gain-map/1.0/") != text.npos) {
    for (const auto name : {"GainMapMin", "GainMapMax", "OffsetSDR", "OffsetHDR",
                            "HDRCapacityMin", "HDRCapacityMax", "BaseRenditionIsHDR"})
      if (xmp_property_value(text, name) != text.npos) return true;
  }
  if (text.find("http://ns.apple.com/HDRGainMap/1.0/") != text.npos &&
      (xmp_property_value(text, "HDRGainMapVersion") != text.npos ||
       xmp_property_value(text, "HDRGainMapHeadroom") != text.npos)) return true;
  if (text.find("http://ns.google.com/photos/1.0/container/") != text.npos) {
    for (auto pos = xmp_property_value(text, "Semantic"); pos != text.npos;
         pos = xmp_property_value(text, "Semantic", pos + 1)) {
      const char delimiter = text[pos++];
      while (pos < text.size() && xml_space(text[pos])) ++pos;
      if (delimiter == '=') {
        if (pos == text.size() || (text[pos] != '\'' && text[pos] != '"')) continue;
        const char quote = text[pos++];
        if (text.substr(pos, 7) == "GainMap" && pos + 7 < text.size() && text[pos + 7] == quote) return true;
      } else if (text.substr(pos, 7) == "GainMap") {
        pos += 7;
        while (pos < text.size() && xml_space(text[pos])) ++pos;
        if (pos < text.size() && text[pos] == '<') return true;
      }
    }
  }
  return false;
}

inline constexpr std::string_view jpeg_xmp_namespace{"http://ns.adobe.com/xap/1.0/\0",29};
inline constexpr std::string_view jpeg_extended_xmp_namespace{"http://ns.adobe.com/xmp/extension/\0",35};
struct JpegXmpSegment { std::span<const std::byte> bytes; std::size_t image_index{}; };

std::optional<std::string> extended_xmp_guid(std::string_view value) {
  if (value.size() != 32) return {};
  std::string guid;
  for (char c : value) {
    if (c >= 'a' && c <= 'f') c = static_cast<char>(c - 'a' + 'A');
    if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) return {};
    guid.push_back(c);
  }
  return guid;
}

// Only Extended XMP transport is reconstructed here, never HDR numbers/XML.
// Packets must be linked by HasExtendedXMP in the same JPEG. Check all ranges
// and complete non-overlapping coverage before allocating a bounded buffer.
std::expected<std::vector<std::vector<std::byte>>, std::string> reassemble_extended_xmp(
    std::span<const JpegXmpSegment> segments) {
  constexpr auto limit = encoding_defaults::codec_metadata_max_bytes;
  constexpr auto marker_limit = encoding_defaults::jpeg_max_saved_metadata_marker_count;
  if (segments.size() > marker_limit) return std::unexpected{"JPEG Extended XMP marker 数量超过上限。"};
  std::size_t raw_bytes = 0;
  for (const auto& segment : segments) {
    if (segment.bytes.size() > limit - raw_bytes) return std::unexpected{"JPEG Extended XMP payload 超过 metadata 上限。"};
    raw_bytes += segment.bytes.size();
  }
  struct Link { std::size_t image; std::string guid; };
  std::vector<Link> links;
  for (const auto& segment : segments) {
    const std::string_view packet{reinterpret_cast<const char*>(segment.bytes.data()),segment.bytes.size()};
    if (!packet.starts_with(jpeg_xmp_namespace) || packet.find("http://ns.adobe.com/xmp/note/") == packet.npos) continue;
    for (auto at = xmp_property_value(packet,"HasExtendedXMP"); at != packet.npos;
         at = xmp_property_value(packet,"HasExtendedXMP",at+1)) {
      auto value = at + 1;
      while (value < packet.size() && xml_space(packet[value])) ++value;
      char end = '<';
      if (packet[at] == '=') {
        if (value == packet.size() || (packet[value] != '\'' && packet[value] != '"')) continue;
        end = packet[value++];
      }
      if (packet.size() - value < 33) continue;
      auto guid = extended_xmp_guid(packet.substr(value,32));
      auto tail = value + 32;
      if (end == '<') while (tail < packet.size() && xml_space(packet[tail])) ++tail;
      if (!guid || tail == packet.size() || packet[tail] != end) continue;
      if (std::ranges::any_of(links,[&](const Link& link) { return link.image == segment.image_index && link.guid == *guid; })) continue;
      if (links.size() == marker_limit) return std::unexpected{"JPEG Extended XMP GUID 数量超过上限。"};
      links.push_back({segment.image_index,std::move(*guid)});
    }
  }
  struct Chunk { std::size_t offset; std::span<const std::byte> bytes; };
  struct Group { std::size_t link, total; std::vector<Chunk> chunks; };
  std::vector<Group> groups;
  std::size_t declared_bytes = 0;
  auto u32 = [](std::span<const std::byte> data, std::size_t pos) {
    std::uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) value = (value << 8) | std::to_integer<unsigned>(data[pos+i]);
    return static_cast<std::size_t>(value);
  };
  for (const auto& segment : segments) {
    const std::string_view packet{reinterpret_cast<const char*>(segment.bytes.data()),segment.bytes.size()};
    if (!packet.starts_with(jpeg_extended_xmp_namespace) || packet.size() < 67) continue;
    auto guid = extended_xmp_guid(packet.substr(35,32));
    if (!guid) continue;
    const auto linked = std::ranges::find_if(links,[&](const Link& link) { return link.image == segment.image_index && link.guid == *guid; });
    if (linked == links.end()) continue; // Unassociated ordinary APP data is not HDR.
    if (packet.size() <= 75) return std::unexpected{"JPEG Extended XMP chunk 头部或数据截断。"};
    const auto total = u32(segment.bytes,67), offset = u32(segment.bytes,71);
    const auto data = segment.bytes.subspan(75);
    if (!total || total > limit || offset > total || data.size() > total - offset)
      return std::unexpected{"JPEG Extended XMP total/offset 越界。"};
    const auto link_index = static_cast<std::size_t>(linked - links.begin());
    auto group = std::ranges::find_if(groups,[&](const Group& value) { return value.link == link_index; });
    if (group == groups.end()) {
      if (total > limit - declared_bytes) return std::unexpected{"JPEG Extended XMP 重组超过 metadata 上限。"};
      declared_bytes += total;
      groups.push_back({link_index,total,{}}); group = groups.end()-1;
    } else if (group->total != total) return std::unexpected{"JPEG Extended XMP total 不一致。"};
    group->chunks.push_back({offset,data});
  }
  std::vector<std::vector<std::byte>> packets;
  for (auto& group : groups) {
    std::ranges::sort(group.chunks,{},&Chunk::offset);
    std::size_t end = 0;
    for (const auto& chunk : group.chunks) {
      if (chunk.offset != end) return std::unexpected{"JPEG Extended XMP chunk 缺失或重叠。"};
      end += chunk.bytes.size();
    }
    if (end != group.total) return std::unexpected{"JPEG Extended XMP packet 不完整。"};
    auto packet = decoder_common::make_byte_buffer(group.total,"JPEG Extended XMP");
    if (!packet) return std::unexpected{packet.error()};
    for (const auto& chunk : group.chunks) std::memcpy(packet->data()+chunk.offset,chunk.bytes.data(),chunk.bytes.size());
    packets.push_back(std::move(*packet));
  }
  return packets;
}

struct JpegGainMapMarkers {
  bool iso{}, legacy{}, mpf{}, extended{}, xmp_over_budget{};
  std::size_t xmp_bytes{};
  std::vector<JpegXmpSegment> xmp_segments;
  bool candidate() const noexcept { return iso || legacy || mpf || extended; }
};

void inspect_jpeg_app(unsigned marker, std::span<const std::byte> bytes, JpegGainMapMarkers& found,
    std::size_t image_index = 0, bool collect_xmp = false) {
  const std::string_view payload{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
  // The patched library owns ISO parsing. XMP reconstruction is delayed until
  // all JPEG candidates are scanned, so valid ISO always wins over bad XMP.
  if (marker == 0xe2) {
    found.iso |= payload.starts_with("urn:iso:std:iso:ts:21496:-1");
    found.mpf |= payload.starts_with(std::string_view{"MPF\0", 4});
  } else if (marker == 0xe1) {
    const bool standard = payload.starts_with(jpeg_xmp_namespace);
    const bool extended = payload.starts_with(jpeg_extended_xmp_namespace);
    if (standard) found.legacy |= xmp_has_legacy_gain_map(payload);
    found.extended |= extended;
    if (collect_xmp && (standard || extended)) {
      if (found.xmp_segments.size() == encoding_defaults::jpeg_max_saved_metadata_marker_count ||
          bytes.size() > encoding_defaults::codec_metadata_max_bytes - found.xmp_bytes) found.xmp_over_budget = true;
      else { found.xmp_segments.push_back({bytes,image_index}); found.xmp_bytes += bytes.size(); }
    }
  }
}

// Bounded APP segments, not an ISO JPEG parser. Entropy is skipped with JPEG
// byte-stuffing/restart rules; a secondary JPEG is inspected only for candidates.
JpegGainMapMarkers jpeg_gain_map_markers(std::span<const std::byte> bytes) {
  JpegGainMapMarkers found;
  if (bytes.size() < 2 || bytes[0] != std::byte{0xff} || bytes[1] != std::byte{0xd8}) return found;
  bool entropy = false, between_images = false;
  std::size_t image_index = 0;
  for (std::size_t pos = 2; pos + 1 < bytes.size();) {
    if (between_images) {
      while (pos + 1 < bytes.size() && (bytes[pos] != std::byte{0xff} || bytes[pos + 1] != std::byte{0xd8})) ++pos;
      if (pos + 1 == bytes.size()) break;
      pos += 2; between_images = false; ++image_index;
      continue;
    }
    if (entropy) {
      while (pos + 1 < bytes.size()) {
        if (bytes[pos] != std::byte{0xff}) { ++pos; continue; }
        const auto next = std::to_integer<unsigned>(bytes[pos + 1]);
        if (next == 0 || (next >= 0xd0 && next <= 0xd7)) { pos += 2; continue; }
        if (next == 0xff) { ++pos; continue; }
        entropy = false; break;
      }
      if (entropy || pos + 1 >= bytes.size()) break;
    }
    if (bytes[pos++] != std::byte{0xff}) break;
    while (pos < bytes.size() && bytes[pos] == std::byte{0xff}) ++pos;
    if (pos == bytes.size()) break;
    const auto marker = std::to_integer<unsigned>(bytes[pos++]);
    if (marker == 0xd9) { between_images = true; continue; }
    if (marker == 0xd8) { ++image_index; continue; }
    if (marker == 1 || (marker >= 0xd0 && marker <= 0xd7)) continue;
    if (pos + 2 > bytes.size()) break;
    const auto length = (std::to_integer<unsigned>(bytes[pos]) << 8) | std::to_integer<unsigned>(bytes[pos + 1]);
    if (length < 2 || length > bytes.size() - pos) break;
    if (marker >= 0xe0 && marker <= 0xef) inspect_jpeg_app(marker, bytes.subspan(pos + 2, length - 2), found,image_index,true);
    pos += length;
    if (marker == 0xda) {
      if (!found.candidate()) return found; // Ordinary JPEG never scans entropy.
      entropy = true;
    }
  }
  if (!found.iso && found.extended && !found.legacy && !found.xmp_over_budget) {
    auto packets = reassemble_extended_xmp(found.xmp_segments);
    // Transport errors or orphaned chunks are not proof of legacy HDR. This
    // bounded helper only detects signatures; ordinary XMP remains opaque.
    if (packets) for (const auto& packet : *packets)
      found.legacy |= xmp_has_legacy_gain_map({reinterpret_cast<const char*>(packet.data()),packet.size()});
  }
  return found;
}

} // namespace gain_map_detail

// Ordinary JPEGs only need their short pre-SOS header inspected. MPF is a
// candidate, not proof: stereo/burst JPEGs may contain no gain map.
bool jpeg_may_contain_gain_map(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  auto read_byte = [&]() { return input.get(); };
  if (read_byte() != 0xff || read_byte() != 0xd8) return false;
  std::vector<std::byte> segment;
  gain_map_detail::JpegGainMapMarkers found;
  for (;;) {
    if (read_byte() != 0xff) return false;
    int marker;
    do { marker = read_byte(); } while (marker == 0xff);
    if (marker < 0 || marker == 0xda || marker == 0xd9) return false;
    if (marker == 1 || (marker >= 0xd0 && marker <= 0xd7)) continue;
    const auto high = read_byte(), low = read_byte();
    if (high < 0 || low < 0) return false;
    const auto length = (high << 8) | low;
    if (length < 2) return false;
    if (marker >= 0xe0 && marker <= 0xef) {
      segment.resize(static_cast<std::size_t>(length) - 2);
      input.read(reinterpret_cast<char*>(segment.data()), length - 2);
      if (!input) return false;
      gain_map_detail::inspect_jpeg_app(static_cast<unsigned>(marker), segment, found);
      if (found.candidate()) return true;
    } else {
      input.seekg(length - 2, std::ios::cur);
      if (!input) return false;
    }
  }
}

std::expected<ImageBuffer, std::string> compose_iso_gain_map(
    const ImageBuffer& base, const ImageBuffer& map, avifGainMap& supplied_metadata,
    std::optional<float> requested_headroom = {}, bool sdr_output = false, std::stop_token stop = {},
    const ResourcePlan& resources = {}, EncodeTimingDiagnostics* timing = nullptr) {
  if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
  if (auto valid = hdr::hdr_detail::validate_rgba(base, "ISO 21496-1 base"); !valid) return std::unexpected{valid.error()};
  if (auto valid = hdr::hdr_detail::validate_rgba(map, "ISO 21496-1 auxiliary"); !valid) return std::unexpected{valid.error()};
  if (base.width > UINT32_MAX || base.height > UINT32_MAX || map.width > UINT32_MAX || map.height > UINT32_MAX)
    return std::unexpected{"Gain Map: ISO 21496-1: 图像尺寸超过 API 范围。"};
  auto color = gain_map_detail::resolve_iso_color(base, stop);
  if (!color) return std::unexpected{color.error()};
  if (map.sample_representation != SampleRepresentation::unorm)
    return std::unexpected{"Gain Map: ISO 21496-1: auxiliary 必须使用整数增益采样。"};
  if (map.geometry) return std::unexpected{"Gain Map: ISO 21496-1: auxiliary 必须先规范化几何布局。"};
  if ((base.bit_depth == 16 && base.planes.front().stride % 2) ||
      (map.bit_depth == 16 && map.planes.front().stride % 2))
    return std::unexpected{"Gain Map: ISO 21496-1: 16-bit stride 未按采样对齐。"};
  if (base.sample_representation == SampleRepresentation::ieee_half_float) {
    // 固定 libavif 的 LINEAR EOTF 会夹 [0,1]，且 ISO 标尺为 203 nit；仅此子集可原域直通。
    // scRGB 的 80 nit、负值及扩展线性范围不能先转到 BT.2020 后套用旧增益图。
    if (base.geometry || !base.source_info || base.source_info->transfer_characteristics != 8 ||
        base.source_info->linear_reference_white_nits != 203.0F)
      return std::unexpected{"Gain Map: ISO 21496-1: 浮点旧增益图需要无几何变换、203 nit 标尺的明确线性 CICP。"};
    for (std::size_t y = 0; y < base.height; ++y) {
      if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
      const auto* row = base.planes.front().bytes.data() + y * base.planes.front().stride;
      for (std::size_t sample = 0; sample < base.width * 4; ++sample) {
        if (sample % 4096 == 0 && stop.stop_requested()) return std::unexpected{"任务已取消。"};
        std::uint16_t bits{};
        std::memcpy(&bits, row + sample * 2, 2);
        if (bits > 0x3c00u)
          return std::unexpected{"Gain Map: ISO 21496-1: 浮点旧增益图只支持非负、有限的 [0,1] RGBA，不能静默裁剪。"};
      }
    }
  } else if (base.sample_representation != SampleRepresentation::unorm) {
    return std::unexpected{"Gain Map: ISO 21496-1: base 采样类型不支持。"};
  }
  if (base.geometry) {
    auto packed = convert_image_pixels(base, PixelConversionRequest{
        .format = PixelFormat::rgba, .bit_depth = base.bit_depth, .preserve_alpha = true}, stop, resources);
    if (!packed) return std::unexpected{"Gain Map: ISO 21496-1: " + packed.error()};
    if (packed->geometry || packed->sample_representation != SampleRepresentation::unorm)
      return std::unexpected{"Gain Map: ISO 21496-1: 无法规范化主图像素。"};
    return compose_iso_gain_map(*packed, map, supplied_metadata, requested_headroom, sdr_output, stop, resources, timing);
  }
  if (base.alpha_mode == AlphaMode::premultiplied)
    return std::unexpected{"Gain Map: ISO 21496-1: premultiplied base 必须先转换为 straight alpha。"};
  // 不改写 decoder 所有的 metadata/image/altICC；局部浅复制只在本调用中借用。
  auto metadata = supplied_metadata;
  avifDiagnostics diag{};
  if (avifGainMapValidateMetadata(&metadata, &diag) != AVIF_RESULT_OK)
    return std::unexpected{std::string{"Gain Map: ISO 21496-1: metadata 无效: "} + diag.error};
  if (auto normalized = gain_map_detail::normalize_iso_alternate(metadata, stop); !normalized)
    return std::unexpected{normalized.error()};
  if (!metadata.useBaseColorSpace && !hdr::hdr_detail::primaries_from_cicp(static_cast<int>(metadata.altColorPrimaries)))
    return std::unexpected{"Gain Map: ISO 21496-1: alternate 合成原色未指定或不支持。"};
  const float base_headroom = static_cast<float>(metadata.baseHdrHeadroom.n) / metadata.baseHdrHeadroom.d;
  const float alt_headroom = static_cast<float>(metadata.alternateHdrHeadroom.n) / metadata.alternateHdrHeadroom.d;
  const float headroom = requested_headroom.value_or(std::max(base_headroom, alt_headroom));
  if (!std::isfinite(headroom) || headroom < 0)
    return std::unexpected{"Gain Map: ISO 21496-1: HDR headroom 无效。"};
  const int output_depth = !sdr_output ? 16 :
      (alt_headroom < base_headroom ? metadata.altDepth > 8 : base.bit_depth > 8) ? 16 : 8;
  const std::size_t sample_bytes = output_depth > 8 ? 2u : 1u;
  const auto largest_sample_bytes = std::max<std::size_t>(sample_bytes, map.bit_depth > 8 ? 2u : 1u);
  const auto byte_limit = std::min<std::uint64_t>(encoding_defaults::effective_max_input_file_bytes(), SIZE_MAX);
  if (base.width > byte_limit / base.height / 4 / largest_sample_bytes ||
      map.width > byte_limit / map.height / 8 || base.width > UINT32_MAX / 4 / largest_sample_bytes ||
      map.planes.front().stride > UINT32_MAX || base.planes.front().stride > UINT32_MAX)
    return std::unexpected{"Gain Map: ISO 21496-1: 像素暂存或 stride 超过运行时上限。"};
  using Image = std::unique_ptr<avifImage, decltype(&avifImageDestroy)>;
  const int map_depth = map.bit_depth == 8 ? 8 : std::clamp(map.source_info ? map.source_info->bit_depth : 12, 10, 12);
  Image gain{avifImageCreate(static_cast<std::uint32_t>(map.width), static_cast<std::uint32_t>(map.height),
      map_depth, AVIF_PIXEL_FORMAT_YUV444), &avifImageDestroy};
  if (!gain) return std::unexpected{"Gain Map: ISO 21496-1: 无法创建 map 像素。"};
  gain->colorPrimaries = AVIF_COLOR_PRIMARIES_UNSPECIFIED;
  gain->transferCharacteristics = AVIF_TRANSFER_CHARACTERISTICS_UNSPECIFIED;
  gain->matrixCoefficients = AVIF_MATRIX_COEFFICIENTS_IDENTITY;
  gain->yuvRange = AVIF_RANGE_FULL;
  avifRGBImage map_rgb{};
  avifRGBImageSetDefaults(&map_rgb, gain.get());
  map_rgb.format = AVIF_RGB_FORMAT_RGBA; map_rgb.depth = map.bit_depth; map_rgb.ignoreAlpha = AVIF_TRUE;
  map_rgb.pixels = reinterpret_cast<std::uint8_t*>(const_cast<std::byte*>(map.planes.front().bytes.data()));
  map_rgb.rowBytes = static_cast<std::uint32_t>(map.planes.front().stride);
  if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
  if (avifImageRGBToYUV(gain.get(), &map_rgb) != AVIF_RESULT_OK)
    return std::unexpected{"Gain Map: ISO 21496-1: map 像素转换失败。"};
  metadata.image = gain.get();
  avifRGBImage base_rgb{};
  base_rgb.width = static_cast<std::uint32_t>(base.width); base_rgb.height = static_cast<std::uint32_t>(base.height);
  base_rgb.format = AVIF_RGB_FORMAT_RGBA; base_rgb.depth = base.bit_depth;
  base_rgb.isFloat = base.sample_representation == SampleRepresentation::ieee_half_float ? AVIF_TRUE : AVIF_FALSE;
  base_rgb.rowBytes = static_cast<std::uint32_t>(base.planes.front().stride);
  base_rgb.pixels = reinterpret_cast<std::uint8_t*>(const_cast<std::byte*>(base.planes.front().bytes.data()));
  avifRGBImage rendered{};
  rendered.format = AVIF_RGB_FORMAT_RGBA; rendered.depth = output_depth;
  avifColorPrimaries output_primaries = AVIF_COLOR_PRIMARIES_BT2020;
  if (sdr_output) {
    output_primaries = static_cast<avifColorPrimaries>(color->primaries);
    if (alt_headroom < base_headroom &&
        hdr::hdr_detail::primaries_from_cicp(static_cast<int>(metadata.altColorPrimaries)))
      output_primaries = metadata.altColorPrimaries;
  }
  const auto output_transfer = static_cast<avifTransferCharacteristics>(
      sdr_output ? AVIF_TRANSFER_CHARACTERISTICS_SRGB : AVIF_TRANSFER_CHARACTERISTICS_SMPTE2084);
  avifContentLightLevelInformationBox clli{};
  if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
  // 仅传递验证等价的色彩标识：useBaseColorSpace=true 时先在原线性 RGB 域应用旧 map，
  // 再由 libavif 转到输出原色；绝不先跨色域变换像素再原样使用旧 map。
  const auto render_started = std::chrono::steady_clock::now();
  awjAvifGainMapExecution execution{};
  const auto applied = awjAvifRGBImageApplyGainMap(&base_rgb,
      static_cast<avifColorPrimaries>(color->primaries),
      static_cast<avifTransferCharacteristics>(color->transfer), &metadata,
      headroom, output_primaries, output_transfer, &rendered, &clli, &diag,
      static_cast<std::uint32_t>(hdr::hdr_detail::tone_worker_budget(resources)),
      [](void* value) noexcept -> avifBool { return static_cast<std::stop_token*>(value)->stop_requested() ? AVIF_TRUE : AVIF_FALSE; },
      &stop, &execution);
  if (timing) {
    timing->gain_map_render_seconds = std::max(0.0, timing->gain_map_render_seconds) +
        std::chrono::duration<double>(std::chrono::steady_clock::now() - render_started).count();
    timing->gain_map_render_workers = std::max(timing->gain_map_render_workers, static_cast<int>(execution.workers_started));
  }
  struct Guard { avifRGBImage& rgb; ~Guard() { avifRGBImageFreePixels(&rgb); } } guard{rendered};
  if (execution.cancelled || stop.stop_requested()) return std::unexpected{"任务已取消。"};
  if (applied != AVIF_RESULT_OK) return std::unexpected{std::string{"Gain Map: ISO 21496-1: 合成失败: "} + diag.error};
  if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
  auto pixels = hdr::hdr_detail::make_rgba_bytes(base.width, base.height, sample_bytes, "ISO 21496-1");
  if (!pixels) return std::unexpected{pixels.error()};
  const auto stride = base.width * 4 * sample_bytes;
  for (std::size_t y = 0; y < base.height; ++y) {
    if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
    std::memcpy(pixels->data() + y * stride, rendered.pixels + y * rendered.rowBytes, stride);
  }
  ImageSourceInfo info{.pixel_format = PixelFormat::rgba, .bit_depth = output_depth,
      .color_primaries = static_cast<int>(output_primaries), .transfer_characteristics = static_cast<int>(output_transfer),
      .matrix_coefficients = sdr_output ? 0 : 9, .color_range = 1,
      .has_hdr_metadata = !sdr_output,
      .color_metadata_source = sdr_output ? "gain-map-iso21496-1-sdr-rendition" : "gain-map-heif-iso21496-1-bt2020-pq",
      .source_has_gain_map = true, .rendition_origin = RenditionOrigin::gain_map_reconstructed};
  if (!sdr_output) info.content_light = HdrContentLightMetadata{clli.maxCLL, clli.maxPALL};
  auto image = hdr::hdr_detail::make_rgba_image(base.width, base.height, std::move(*pixels), base.alpha_mode,
      output_depth, SampleRepresentation::unorm, std::move(info), "ISO 21496-1");
  if (!image) return image;
  for (const auto& block : base.metadata) if (block.kind == MetadataKind::exif) image->metadata.push_back(block);
  return image;
}

std::expected<ImageDecodeResult, std::string> render_iso_gain_map(
    ImageBuffer base, const ImageBuffer& map, avifGainMap& supplied_metadata,
    GainMapDecodeMode mode, std::stop_token stop, const ResourcePlan& resources, EncodeTimingDiagnostics* timing) {
  if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
  if (auto valid = hdr::hdr_detail::validate_rgba(base, "ISO 21496-1 base"); !valid) return std::unexpected{valid.error()};
  if (auto valid = hdr::hdr_detail::validate_rgba(map, "ISO 21496-1 auxiliary"); !valid) return std::unexpected{valid.error()};
  if (base.width > UINT32_MAX || base.height > UINT32_MAX || map.width > UINT32_MAX || map.height > UINT32_MAX)
    return std::unexpected{"Gain Map: ISO 21496-1: 图像尺寸超过 API 范围。"};
  auto metadata = supplied_metadata;
  avifDiagnostics diag{};
  if (avifGainMapValidateMetadata(&metadata, &diag) != AVIF_RESULT_OK)
    return std::unexpected{std::string{"Gain Map: ISO 21496-1: metadata 无效: "} + diag.error};
  auto color = gain_map_detail::resolve_iso_color(base, stop);
  if (!color) return std::unexpected{color.error()};
  if (auto normalized = gain_map_detail::normalize_iso_alternate(metadata, stop); !normalized)
    return std::unexpected{normalized.error()};
  // 先建立唯一有效解释，再判定 SDR/HDR 和保留原端点；否则 ICC-only SDR 会被遗漏。
  gain_map_detail::normalize_iso_base(base, *color);
  const float b = static_cast<float>(metadata.baseHdrHeadroom.n) / metadata.baseHdrHeadroom.d;
  const float a = static_cast<float>(metadata.alternateHdrHeadroom.n) / metadata.alternateHdrHeadroom.d;
  const auto high = std::max(b, a), low = std::min(b, a);
  if (!std::isfinite(high) || !std::isfinite(low))
    return std::unexpected{"Gain Map: ISO 21496-1: headroom 无效。"};
  auto hdr_signal = hdr::has_explicit_hdr_signal(base);
  if (!hdr_signal) return std::unexpected{"Gain Map: ISO 21496-1: " + hdr_signal.error()};
  const bool base_is_hdr = *hdr_signal;
  const bool base_is_sdr = !base_is_hdr && base.source_info &&
      base.source_info->transfer_characteristics &&
      *base.source_info->transfer_characteristics != 0 && *base.source_info->transfer_characteristics != 2;
  const auto mark_source = [](ImageBuffer& image) {
    if (image.source_info) {
      image.source_info->source_has_gain_map = true;
      image.source_info->rendition_origin = RenditionOrigin::gain_map_base;
    }
  };
  // 没有 SDR 端点时仍交回 HDR，让目标格式沿用明确的 tone-map，不能把低 HDR 端点夹成 SDR。
  if ((mode == GainMapDecodeMode::sdr_only || high == 0) && low == 0) {
    if (b == low && base_is_sdr) {
      if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
      mark_source(base);
      return ImageDecodeResult{.image = std::move(base)};
    }
    auto sdr = compose_iso_gain_map(base, map, metadata, low, true, stop, resources, timing);
    if (!sdr) return std::unexpected{sdr.error()};
    if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
    return ImageDecodeResult{.image = std::move(*sdr)};
  }
  ImageDecodeResult output;
  const bool keep_both = mode == GainMapDecodeMode::preserve_both && high > low;
  if (b == high && base_is_hdr) {
    if (keep_both) {
      auto other = compose_iso_gain_map(base, map, metadata, low, low == 0, stop, resources, timing);
      if (!other) return std::unexpected{other.error()};
      output.gain_map_alternative = GainMapAlternative{std::move(*other), high, low};
    }
    mark_source(base);
    output.image = std::move(base);
  } else {
    auto primary = compose_iso_gain_map(base, map, metadata, high, high == 0, stop, resources, timing);
    if (!primary) return std::unexpected{primary.error()};
    output.image = std::move(*primary);
    if (keep_both) {
      if (b == low && base_is_sdr) {
        mark_source(base);
        output.gain_map_alternative = GainMapAlternative{std::move(base), high, low};
      } else {
        auto other = compose_iso_gain_map(base, map, metadata, low, low == 0, stop, resources, timing);
        if (!other) return std::unexpected{other.error()};
        output.gain_map_alternative = GainMapAlternative{std::move(*other), high, low};
      }
    }
  }
  if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
  return output;
}

std::expected<IsoGainMapPtr, std::string> compute_iso_gain_map(
    const ImageBuffer& main, const GainMapAlternative& alternative,
    std::uint32_t map_width, std::uint32_t map_height, int map_depth, std::stop_token stop,
    const ResourcePlan& resources, EncodeTimingDiagnostics* timing) {
  const auto compute_started = std::chrono::steady_clock::now();
  if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
  if (main.width != alternative.image.width || main.height != alternative.image.height ||
      !main.width || !main.height || main.width > UINT32_MAX || main.height > UINT32_MAX ||
      (map_depth != 8 && map_depth != 10 && map_depth != 12))
    return std::unexpected{"Gain Map: ISO 21496-1: 两端尺寸或增益图位深无效。"};
  const float main_headroom = alternative.main_hdr_headroom_log2;
  const float alt_headroom = alternative.alternate_hdr_headroom_log2;
  if (!std::isfinite(main_headroom) || !std::isfinite(alt_headroom) || main_headroom < 0 || alt_headroom < 0)
    return std::unexpected{"Gain Map: ISO 21496-1: 显式端点 headroom 无效。"};
  // 先限制 metadata 并验证 ICC，避免布局转换先复制巨量 payload 或掩盖浮点 ICC。
  auto main_color = gain_map_detail::resolve_iso_color(main, stop);
  if (!main_color) return std::unexpected{main_color.error()};
  auto alt_color = gain_map_detail::resolve_iso_color(alternative.image, stop);
  if (!alt_color) return std::unexpected{alt_color.error()};
  std::optional<ImageBuffer> packed_main, packed_alt;
  const auto pack = [&](const ImageBuffer& source, std::optional<ImageBuffer>& storage)
      -> std::expected<const ImageBuffer*, std::string> {
    if (source.pixel_format == PixelFormat::rgba && !source.geometry &&
        source.sample_representation == SampleRepresentation::unorm) return &source;
    auto converted = convert_image_pixels(source, PixelConversionRequest{
        .format = PixelFormat::rgba, .bit_depth = source.bit_depth > 8 ? 16 : 8, .preserve_alpha = true}, stop, resources);
    if (!converted) return std::unexpected{"Gain Map: ISO 21496-1: " + converted.error()};
    storage = std::move(*converted);
    return &*storage;
  };
  auto primary = pack(main, packed_main);
  if (!primary) return std::unexpected{primary.error()};
  auto secondary = pack(alternative.image, packed_alt);
  if (!secondary) return std::unexpected{secondary.error()};
  // 非 ICC 的浮点转换可能改变原色/传递函数。这里生成新 map，因此使用转换后的解释。
  if (packed_main && !main_color->verified_icc) {
    main_color = gain_map_detail::resolve_iso_color(*packed_main, stop);
    if (!main_color) return std::unexpected{main_color.error()};
  }
  if (packed_alt && !alt_color->verified_icc) {
    alt_color = gain_map_detail::resolve_iso_color(*packed_alt, stop);
    if (!alt_color) return std::unexpected{alt_color.error()};
  }
  const auto view = [](const ImageBuffer& source) -> std::expected<avifRGBImage, std::string> {
    if (auto valid = hdr::hdr_detail::validate_rgba(source, "ISO 21496-1 encode"); !valid)
      return std::unexpected{valid.error()};
    if (source.planes.front().stride > UINT32_MAX || (source.bit_depth == 16 && source.planes.front().stride % 2) ||
        source.sample_representation != SampleRepresentation::unorm || source.alpha_mode == AlphaMode::premultiplied || source.geometry)
      return std::unexpected{"Gain Map: ISO 21496-1: 编码端点布局、采样类型、alpha 或 stride 无效。"};
    avifRGBImage rgb{};
    rgb.width = static_cast<std::uint32_t>(source.width); rgb.height = static_cast<std::uint32_t>(source.height);
    rgb.format = AVIF_RGB_FORMAT_RGBA; rgb.depth = source.bit_depth;
    rgb.rowBytes = static_cast<std::uint32_t>(source.planes.front().stride);
    rgb.pixels = reinterpret_cast<std::uint8_t*>(const_cast<std::byte*>(source.planes.front().bytes.data()));
    return rgb;
  };
  // 这些 view 只借用调用参数或本作用域 packed_* 的像素，不向外返回。
  auto main_view = view(**primary);
  if (!main_view) return std::unexpected{main_view.error()};
  auto alt_view = view(**secondary);
  if (!alt_view) return std::unexpected{alt_view.error()};
  const auto byte_limit = std::min<std::uint64_t>(encoding_defaults::effective_max_input_file_bytes(), SIZE_MAX);
  if (main.width > byte_limit / main.height / (3 * sizeof(float)))
    return std::unexpected{"Gain Map: ISO 21496-1: 增益图运算暂存超过运行时上限。"};
  if (!map_width) map_width = static_cast<std::uint32_t>(std::max<std::size_t>(1, main.width / 4));
  if (!map_height) map_height = static_cast<std::uint32_t>(std::max<std::size_t>(1, main.height / 4));
  if (map_width > main.width || map_height > main.height ||
      map_width > UINT32_MAX / 4 / (map_depth > 8 ? 2u : 1u))
    return std::unexpected{"Gain Map: ISO 21496-1: 增益图尺寸超出主图或 RGB stride API 范围。"};
  IsoGainMapPtr map{avifGainMapCreate(), &avifGainMapDestroy};
  if (!map) return std::unexpected{"Gain Map: ISO 21496-1: 无法创建编码 metadata。"};
  map->image = avifImageCreate(map_width, map_height, map_depth, AVIF_PIXEL_FORMAT_YUV444);
  if (!map->image) return std::unexpected{"Gain Map: ISO 21496-1: 无法创建编码增益图。"};
  map->image->matrixCoefficients = AVIF_MATRIX_COEFFICIENTS_IDENTITY;
  map->image->yuvRange = AVIF_RANGE_FULL;
  avifDiagnostics diag{};
  if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
  awjAvifGainMapExecution execution{};
  const auto result = awjAvifRGBImageComputeGainMap(&*main_view,
      static_cast<avifColorPrimaries>(main_color->primaries),
      static_cast<avifTransferCharacteristics>(main_color->transfer), &*alt_view,
      static_cast<avifColorPrimaries>(alt_color->primaries),
      static_cast<avifTransferCharacteristics>(alt_color->transfer), map.get(), &diag,
      static_cast<std::uint32_t>(hdr::hdr_detail::tone_worker_budget(resources)),
      [](void* value) noexcept -> avifBool { return static_cast<std::stop_token*>(value)->stop_requested() ? AVIF_TRUE : AVIF_FALSE; },
      &stop, &execution);
  if (timing) {
    timing->gain_map_compute_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - compute_started).count();
    timing->gain_map_compute_workers = static_cast<int>(execution.workers_started);
  }
  if (execution.cancelled || stop.stop_requested()) return std::unexpected{"任务已取消。"};
  if (result != AVIF_RESULT_OK)
    return std::unexpected{std::string{"Gain Map: ISO 21496-1: 重新生成增益图失败: "} + diag.error};
  if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
  if (main_headroom != alt_headroom) {
    // 呈现容量不等同于这张图片的实际峰值。暗 HDR 也必须保留原容器的端点方向。
    const bool computed_backwards = static_cast<double>(map->alternateHdrHeadroom.n) / map->alternateHdrHeadroom.d <
        static_cast<double>(map->baseHdrHeadroom.n) / map->baseHdrHeadroom.d;
    if (computed_backwards != (alt_headroom < main_headroom)) {
      for (int c = 0; c < 3; ++c) {
        if (map->gainMapGamma[c].n != map->gainMapGamma[c].d ||
            map->gainMapMin[c].n == INT32_MIN || map->gainMapMax[c].n == INT32_MIN)
          return std::unexpected{"Gain Map: ISO 21496-1: 无法安全反转生成的增益图。"};
        const auto old_min = map->gainMapMin[c];
        const auto old_max = map->gainMapMax[c];
        map->gainMapMin[c] = {-old_max.n, old_max.d};
        map->gainMapMax[c] = {-old_min.n, old_min.d};
        const unsigned sample_bytes = map_depth > 8 ? 2u : 1u;
        const auto max_sample = static_cast<std::uint16_t>((1u << map_depth) - 1u);
        for (std::uint32_t y = 0; y < map_height; ++y) {
          if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
          auto* row = map->image->yuvPlanes[c] + static_cast<std::size_t>(y) * map->image->yuvRowBytes[c];
          for (std::uint32_t x = 0; x < map_width; ++x) {
            if (sample_bytes == 1) row[x] = static_cast<std::uint8_t>(max_sample - row[x]);
            else {
              std::uint16_t value{};
              std::memcpy(&value, row + static_cast<std::size_t>(x) * 2, 2);
              value = static_cast<std::uint16_t>(max_sample - value);
              std::memcpy(row + static_cast<std::size_t>(x) * 2, &value, 2);
            }
          }
        }
      }
    }
    const auto fraction = [](float value) -> std::optional<avifUnsignedFraction> {
      if (static_cast<double>(value) > UINT32_MAX) return {};
      const auto denominator = static_cast<std::uint32_t>(value == 0 ? 1.0 :
          std::max(1.0, std::min(1'048'576.0, std::floor(static_cast<double>(UINT32_MAX) / value))));
      const auto numerator = std::round(static_cast<double>(value) * denominator);
      if (numerator < 0 || numerator > UINT32_MAX) return {};
      return avifUnsignedFraction{static_cast<std::uint32_t>(numerator), denominator};
    };
    const auto b = fraction(main_headroom), a = fraction(alt_headroom);
    if (!b || !a) return std::unexpected{"Gain Map: ISO 21496-1: 端点 headroom 超出分数表示范围。"};
    map->baseHdrHeadroom = *b;
    map->alternateHdrHeadroom = *a;
  }
  map->altColorPrimaries = static_cast<avifColorPrimaries>(alt_color->primaries);
  map->altTransferCharacteristics = static_cast<avifTransferCharacteristics>(alt_color->transfer);
  map->altMatrixCoefficients = AVIF_MATRIX_COEFFICIENTS_IDENTITY;
  map->altYUVRange = AVIF_RANGE_FULL;
  map->altDepth = static_cast<std::uint32_t>((**secondary).bit_depth);
  // pixi 只描述颜色平面，alpha 由主图的独立辅助项共享，不计为第四个颜色通道。
  map->altPlaneCount = 3;
  const auto& alt_info = (**secondary).source_info;
  if (alt_info && alt_info->content_light)
    map->altCLLI = {alt_info->content_light->max_cll, alt_info->content_light->max_pall};
  if (avifGainMapValidateMetadata(map.get(), &diag) != AVIF_RESULT_OK)
    return std::unexpected{std::string{"Gain Map: ISO 21496-1: 编码 metadata 无效: "} + diag.error};
  if (timing) timing->gain_map_compute_seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - compute_started).count();
  return std::move(map);
}

namespace gain_map_detail {

using Decoder = std::unique_ptr<uhdr_codec_private_t, decltype(&uhdr_release_decoder)>;
struct ProbedJpeg {
  Decoder decoder{nullptr, &uhdr_release_decoder};
  ImageDimensions dimensions;
  Decoder sdr_decoder{nullptr, &uhdr_release_decoder};
  GainMapDecodeMode mode{GainMapDecodeMode::hdr_only};
  float hdr_headroom_log2{};
};

// The same gate is used for file, memory and direct-codec entry points. MPF
// alone is ordinary MPO; legacy HDR is a terminal error, never not-my-format.
std::expected<std::optional<ProbedJpeg>, std::string> probe_jpeg(
    std::span<const std::byte> bytes, std::stop_token stop,
    GainMapDecodeMode mode = GainMapDecodeMode::hdr_only) {
  const auto markers = jpeg_gain_map_markers(bytes);
  if (!markers.iso) {
    if (markers.legacy) return std::unexpected{std::string{legacy_gain_map_error}};
    if (markers.extended && markers.xmp_over_budget)
      return std::unexpected{"Gain Map: ISO 21496-1: Extended XMP 超过安全检查预算，不能安全排除旧 HDR。"};
    return std::optional<ProbedJpeg>{};
  }
  if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
  uhdr_compressed_image_t input{.data = const_cast<std::byte*>(bytes.data()), .data_sz = bytes.size(),
      .capacity = bytes.size(), .cg = UHDR_CG_UNSPECIFIED,
      .ct = UHDR_CT_UNSPECIFIED, .range = UHDR_CR_UNSPECIFIED};
  const auto make_decoder = [&](bool sdr) -> std::expected<Decoder, std::string> {
    Decoder decoder{uhdr_create_decoder(), &uhdr_release_decoder};
    if (!decoder) return std::unexpected{"Gain Map: ISO 21496-1: 无法创建 decoder。"};
    for (const auto error : {uhdr_dec_set_image(decoder.get(), &input),
         uhdr_dec_set_out_img_format(decoder.get(), sdr ? UHDR_IMG_FMT_32bppRGBA8888 : UHDR_IMG_FMT_64bppRGBAHalfFloat),
         uhdr_dec_set_out_color_transfer(decoder.get(), sdr ? UHDR_CT_SRGB : UHDR_CT_LINEAR)}) {
      if (error.error_code != UHDR_CODEC_OK) return std::unexpected{uhdr_error(error)};
    }
    const auto error = uhdr_dec_probe(decoder.get());
    if (error.error_code != UHDR_CODEC_OK) return std::unexpected{uhdr_error(error)};
    return std::move(decoder);
  };
  auto decoder = make_decoder(mode == GainMapDecodeMode::sdr_only);
  if (!decoder) return std::unexpected{decoder.error()};
  auto* metadata = uhdr_dec_get_gainmap_metadata(decoder->get());
  if (!metadata || !std::isfinite(metadata->hdr_capacity_max) || metadata->hdr_capacity_max < 1)
    return std::unexpected{"Gain Map: ISO 21496-1: 缺少有效 HDR capacity。"};
  const float hdr_headroom = std::log2(metadata->hdr_capacity_max);
  // capacity=1 的全 SDR 边界不伪装成线性 HDR，也不制造同端点的双呈现。
  if (hdr_headroom == 0 && mode != GainMapDecodeMode::sdr_only)
    return probe_jpeg(bytes, stop, GainMapDecodeMode::sdr_only);
  auto dimensions = decoder_common::make_image_dimensions_checked(
      uhdr_dec_get_image_width(decoder->get()), uhdr_dec_get_image_height(decoder->get()), "ISO 21496-1");
  if (!dimensions) return std::unexpected{dimensions.error()};
  dimensions->source_bit_depth = hdr_headroom > 0 ? 16 : 8;
  dimensions->source_dynamic_range = hdr_headroom > 0 ? ImageDynamicRangeHint::hdr : ImageDynamicRangeHint::sdr;
  dimensions->source_has_gain_map = true;
  dimensions->gain_map_width = static_cast<std::uint32_t>(std::max(0, uhdr_dec_get_gainmap_width(decoder->get())));
  dimensions->gain_map_height = static_cast<std::uint32_t>(std::max(0, uhdr_dec_get_gainmap_height(decoder->get())));
  dimensions->decoded_storage_bytes = static_cast<std::uint64_t>(dimensions->width) * dimensions->height * 8;
  Decoder secondary{nullptr, &uhdr_release_decoder};
  if (mode == GainMapDecodeMode::preserve_both) {
    auto sdr = make_decoder(true);
    if (!sdr) return std::unexpected{sdr.error()};
    secondary = std::move(*sdr);
  }
  return std::optional<ProbedJpeg>{ProbedJpeg{.decoder = std::move(*decoder), .dimensions = *dimensions,
      .sdr_decoder = std::move(secondary), .mode = mode, .hdr_headroom_log2 = hdr_headroom}};
}

// uhdr_dec_set_image owns a compressed copy; release our read buffer after probe.
std::expected<std::optional<ProbedJpeg>, std::string> probe_jpeg(
    const fs::path& path, std::stop_token stop,
    GainMapDecodeMode mode = GainMapDecodeMode::hdr_only) {
  // 使用 JPEG 签名，不能靠改后缀绕过 Gain Map 的终止错误。
  if (!jpeg_may_contain_gain_map(path)) return std::optional<ProbedJpeg>{};
  auto bytes = decoder_common::read_file_bytes(path, "ISO 21496-1");
  if (!bytes) return std::unexpected{bytes.error()};
  return probe_jpeg(std::span<const std::byte>{*bytes}, stop, mode);
}

std::expected<std::optional<ImageDimensions>, std::string> oriented_dimensions(
    std::expected<std::optional<ProbedJpeg>, std::string> probed) {
  if (!probed) return std::unexpected{probed.error()};
  if (!*probed) return std::optional<ImageDimensions>{};
  auto dimensions = (**probed).dimensions;
  if (auto* exif = uhdr_dec_get_exif((**probed).decoder.get()); exif && exif->data && exif->data_sz) {
    auto* begin = static_cast<const std::uint8_t*>(exif->data);
    auto size = exif->data_sz;
    if (size >= 6 && std::memcmp(begin, "Exif\0\0", 6) == 0) { begin += 6; size -= 6; }
    std::size_t offset{};
    if (avifGetExifOrientationOffset(begin, size, &offset) == AVIF_RESULT_OK && offset < size) {
      const auto orientation = begin[offset];
      if (orientation < 1 || orientation > 8) return std::unexpected{"Gain Map: ISO 21496-1: Exif orientation 无效。"};
      if (orientation >= 5) std::swap(dimensions.width, dimensions.height);
    }
  }
  return std::optional<ImageDimensions>{dimensions};
}

std::expected<std::optional<ImageDecodeResult>, std::string> decode_probed_jpeg(
    std::expected<std::optional<ProbedJpeg>, std::string> probed, std::stop_token stop, int decode_threads) {
  if (!probed) return std::unexpected{probed.error()};
  if (!*probed) return std::optional<ImageDecodeResult>{};
  const auto previous_threads = awjUhdrThreadLimit(static_cast<unsigned int>(std::max(1, decode_threads)));
  struct ThreadBudget { unsigned int previous; ~ThreadBudget() { awjUhdrThreadLimit(previous); } } thread_budget{previous_threads};
  auto& descriptor = **probed;
  auto& decoder = descriptor.decoder;
  const auto dimensions = descriptor.dimensions;
  const bool sdr = descriptor.mode == GainMapDecodeMode::sdr_only;
  const std::size_t sample_bytes = sdr ? 1u : 2u;
  const auto stride = static_cast<std::size_t>(dimensions.width) * 4 * sample_bytes;
  auto pixels = hdr::hdr_detail::make_rgba_bytes(dimensions.width, dimensions.height, sample_bytes, "ISO 21496-1");
  if (!pixels) return std::unexpected{pixels.error()};
  const auto error = uhdr_decode(decoder.get());
  if (error.error_code != UHDR_CODEC_OK) return std::unexpected{uhdr_error(error)};
  if (stop.stop_requested()) return std::unexpected{"任务已取消。"};
  auto* raw = uhdr_get_decoded_image(decoder.get());
  if (!raw) return std::unexpected{"Gain Map: ISO 21496-1: decoder 未提供输出图像。"};
  // 固定 libultrahdr 的 SDR 分支用 copy_raw_image 复制 JPEG 结果，ct 会被覆盖成 UNSPECIFIED。
  // 此时传递特性来自已验证的 SRGB 输出请求；HDR 输出仍严格要求 LINEAR。
  const bool valid_transfer = sdr ? (raw->ct == UHDR_CT_SRGB || raw->ct == UHDR_CT_UNSPECIFIED)
                                  : raw->ct == UHDR_CT_LINEAR;
  if (raw->fmt != (sdr ? UHDR_IMG_FMT_32bppRGBA8888 : UHDR_IMG_FMT_64bppRGBAHalfFloat) ||
      !valid_transfer ||
      raw->w != dimensions.width || raw->h != dimensions.height || !raw->planes[0] || raw->stride[0] < raw->w)
    return std::unexpected{std::format("Gain Map: ISO 21496-1: decoder 输出布局无效：mode={} format={} transfer={} gamut={} size={}x{} stride={}，预期 {}x{}。",
        sdr ? "sdr" : "hdr", static_cast<int>(raw->fmt), static_cast<int>(raw->ct), static_cast<int>(raw->cg),
        raw->w, raw->h, raw->stride[0], dimensions.width, dimensions.height)};
  const int primaries = raw->cg == UHDR_CG_BT_709 ? 1 : raw->cg == UHDR_CG_DISPLAY_P3 ? 12
      : raw->cg == UHDR_CG_BT_2100 ? 9 : 0;
  if (!primaries) return std::unexpected{"Gain Map: ISO 21496-1: decoder 未提供输出原色。"};
  for (std::size_t y = 0; y < dimensions.height; ++y)
    std::memcpy(pixels->data() + y * stride,
        static_cast<const std::byte*>(raw->planes[0]) + y * raw->stride[0] * 4 * sample_bytes, stride);
  ImageSourceInfo info{.pixel_format = PixelFormat::rgba, .bit_depth = sdr ? 8 : 16,
      .color_primaries = primaries, .transfer_characteristics = sdr ? 13 : 8,
      .matrix_coefficients = 0, .color_range = 1, .has_hdr_metadata = !sdr,
      .color_metadata_source = sdr ? "gain-map-iso21496-1-sdr-rendition" : "gain-map-iso21496-1-linear",
      .source_has_gain_map = true,
      .rendition_origin = sdr ? RenditionOrigin::gain_map_base : RenditionOrigin::gain_map_reconstructed};
  if (!sdr) info.linear_reference_white_nits = 203.0F;
  auto image = hdr::hdr_detail::make_rgba_image(dimensions.width, dimensions.height,
      std::move(*pixels), AlphaMode::none, sdr ? 8 : 16,
      sdr ? SampleRepresentation::unorm : SampleRepresentation::ieee_half_float, std::move(info), "ISO 21496-1");
  if (!image) return std::unexpected{image.error()};
  // 源 ICC 不能贴给重建的线性 HDR；由 decoder 输出的原色与传递函数解释像素。
  if (auto* exif = uhdr_dec_get_exif(decoder.get()); exif && exif->data && exif->data_sz) {
    const auto* begin = static_cast<const std::byte*>(exif->data);
    if (exif->data_sz >= 6 && std::memcmp(begin, "Exif\0\0", 6) == 0) begin += 6;
    image->metadata.push_back({MetadataKind::exif,
        {begin, static_cast<const std::byte*>(exif->data) + exif->data_sz}});
  }
  if (auto oriented = apply_exif_orientation(*image); !oriented) return std::unexpected{oriented.error()};
  ImageDecodeResult output{.image = std::move(*image),
      .decoder_id = sdr ? "iso21496-1-jpeg-gain-map-sdr" : "iso21496-1-jpeg-gain-map-enhanced"};
  if (descriptor.sdr_decoder) {
    const auto headroom = descriptor.hdr_headroom_log2;
    ProbedJpeg low{.decoder = std::move(descriptor.sdr_decoder), .dimensions = dimensions,
        .mode = GainMapDecodeMode::sdr_only};
    // HDR 数值和 Exif 已拥有，先释放 HDR 引擎大缓冲再解 SDR，避免两套引擎像素重叠驻留。
    decoder.reset();
    std::expected<std::optional<ProbedJpeg>, std::string> low_probe{std::optional<ProbedJpeg>{std::move(low)}};
    auto other = decode_probed_jpeg(std::move(low_probe), stop, decode_threads);
    if (!other) return std::unexpected{other.error()};
    if (!*other || (**other).image.width != output.image.width || (**other).image.height != output.image.height)
      return std::unexpected{"Gain Map: ISO 21496-1: HDR/SDR 两端方向或尺寸不一致。"};
    output.gain_map_alternative = GainMapAlternative{std::move((**other).image), headroom, 0.0F};
  }
  return std::optional<ImageDecodeResult>{std::move(output)};
}

} // namespace gain_map_detail

std::expected<std::optional<ImageDimensions>, std::string> probe_jpeg_gain_map_dimensions(
    const fs::path& path, std::stop_token stop = {}) {
  return gain_map_detail::oriented_dimensions(gain_map_detail::probe_jpeg(path, stop));
}

std::expected<std::optional<ImageDimensions>, std::string> probe_jpeg_gain_map_dimensions(
    std::span<const std::byte> bytes, std::stop_token stop = {}) {
  return gain_map_detail::oriented_dimensions(gain_map_detail::probe_jpeg(bytes, stop));
}

// A recognized legacy or invalid ISO map is an error, never an SDR fallback.
std::expected<std::optional<ImageDecodeResult>, std::string> decode_jpeg_gain_map(
    const fs::path& path, std::stop_token stop = {}, int decode_threads = 1,
    GainMapDecodeMode mode = GainMapDecodeMode::hdr_only) {
  return gain_map_detail::decode_probed_jpeg(gain_map_detail::probe_jpeg(path, stop, mode), stop, decode_threads);
}

std::expected<std::optional<ImageDecodeResult>, std::string> decode_jpeg_gain_map(
    std::span<const std::byte> bytes, std::stop_token stop = {}, int decode_threads = 1,
    GainMapDecodeMode mode = GainMapDecodeMode::hdr_only) {
  return gain_map_detail::decode_probed_jpeg(gain_map_detail::probe_jpeg(bytes, stop, mode), stop, decode_threads);
}

} // namespace awj
