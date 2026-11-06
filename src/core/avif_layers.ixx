module;
#include "../compat/stdlib_compat.h"
#include <array>
#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module awj.avif_layers;

export namespace awj {

inline constexpr int max_avif_layers = 4;

struct AvifLayerOptions {
  std::optional<std::pair<std::uint32_t, std::uint32_t>> size{};
  std::optional<std::pair<std::uint32_t, std::uint32_t>> scale{};
  std::optional<int> quality{};
  std::optional<int> alpha_quality{};
  bool specified() const noexcept { return size || scale || quality || alpha_quality; }
};

struct AvifLayerConfig {
  bool automatic{};
  std::optional<int> count{};
  std::array<AvifLayerOptions, max_avif_layers> layers{};
  bool enabled() const noexcept { return automatic || count.has_value(); }
};

struct AvifLayerPlan {
  std::uint32_t width{};
  std::uint32_t height{};
  int quality{};
  int alpha_quality{};
};

namespace avif_layer_detail {
awj::compat::expected<std::uint32_t, std::string> number(std::wstring_view text,
                                                       std::uint32_t maximum) {
  if (text.empty() || text.size() > 10) return awj::compat::unexpected{"渐进层参数需要合法正整数。"};
  std::uint64_t value{};
  for (const auto ch : text) {
    if (ch < L'0' || ch > L'9') return awj::compat::unexpected{"渐进层参数需要合法正整数。"};
    value = value * 10 + static_cast<unsigned>(ch - L'0');
    if (value > maximum) return awj::compat::unexpected{"渐进层参数超过允许范围。"};
  }
  if (!value) return awj::compat::unexpected{"渐进层参数不能为 0。"};
  return static_cast<std::uint32_t>(value);
}
}

awj::compat::expected<void, std::string> set_avif_layer_option(
    AvifLayerConfig& config, std::wstring_view option, std::wstring_view text) {
  const auto colon = text.find(L':');
  if (colon == std::wstring_view::npos) return awj::compat::unexpected{"渐进层参数格式为 序号:值，序号从 1 开始。"};
  auto index = avif_layer_detail::number(text.substr(0, colon), max_avif_layers);
  if (!index) return awj::compat::unexpected{index.error()};
  auto& layer = config.layers[*index - 1];
  const auto value = text.substr(colon + 1);
  if (option == L"--avif-layer-size" || option == L"--avif-layer-scale") {
    const bool size = option == L"--avif-layer-size";
    const auto separator = value.find(size ? L'x' : L'/');
    if (separator == std::wstring_view::npos)
      return awj::compat::unexpected{"层尺寸使用 序号:宽x高，层比例使用 序号:分子/分母。"};
    auto a = avif_layer_detail::number(value.substr(0, separator), size ? 65536 : 1000000);
    auto b = avif_layer_detail::number(value.substr(separator + 1), size ? 65536 : 1000000);
    if (!a || !b) return awj::compat::unexpected{"渐进层尺寸或比例无效。"};
    if (layer.size || layer.scale) return awj::compat::unexpected{"每层只能指定一次尺寸或比例，不能混填。"};
    if (!size && *a > *b) return awj::compat::unexpected{"渐进层比例不能大于最终尺寸。"};
    if (size) layer.size = std::pair{*a, *b};
    else layer.scale = std::pair{*a, *b};
  } else {
    auto quality = avif_layer_detail::number(value, 100);
    if (!quality) return awj::compat::unexpected{quality.error()};
    auto& target = option == L"--avif-layer-quality" ? layer.quality : layer.alpha_quality;
    if (target) return awj::compat::unexpected{"同一层的质量参数不能重复指定。"};
    target = static_cast<int>(*quality);
  }
  return {};
}

awj::compat::expected<void, std::string> validate_avif_layers(
    const AvifLayerConfig& config, bool avif_output, int quality,
    std::optional<int> visual_quality = {}) {
  bool any{};
  for (const auto& layer : config.layers) any = any || layer.specified();
  if ((config.enabled() || any) && !avif_output)
    return awj::compat::unexpected{"AVIF 渐进层参数仅支持 AVIF 输出。"};
  if (config.automatic && (config.count || any))
    return awj::compat::unexpected{"自动渐进与手动分层参数不能同时使用。"};
  if (config.automatic && visual_quality.value_or(quality) < 10)
    return awj::compat::unexpected{"自动渐进沿用 avifenc 参数，目标质量不能低于 10；可使用手动层质量。"};
  if (any && !config.count) return awj::compat::unexpected{"手动层参数需要 --avif-layers 指定总层数。"};
  if (config.count) {
    if (*config.count < 2 || *config.count > max_avif_layers)
      return awj::compat::unexpected{"AVIF 渐进总层数必须为 2～4。"};
    for (int i = 0; i < max_avif_layers; ++i) {
      const auto& layer = config.layers[i];
      if (i >= *config.count && layer.specified())
        return awj::compat::unexpected{"渐进层序号超过总层数。"};
      if (layer.size && layer.scale) return awj::compat::unexpected{"同层尺寸与比例不能混填。"};
      if ((layer.quality && (*layer.quality < 1 || *layer.quality > 100)) ||
          (layer.alpha_quality && (*layer.alpha_quality < 1 || *layer.alpha_quality > 100)))
        return awj::compat::unexpected{"渐进层质量必须为 1～100。"};
      if (i < *config.count - 1 && !layer.specified())
        return awj::compat::unexpected{"每个预览层至少需要明确指定尺寸、比例或质量。"};
    }
    if (visual_quality && config.layers[*config.count - 1].quality)
      return awj::compat::unexpected{"视觉质量模式不能同时固定手动最后一层的颜色质量。"};
  }
  return {};
}

awj::compat::expected<std::vector<AvifLayerPlan>, std::string> plan_avif_layers(
    const AvifLayerConfig& config, std::uint32_t width, std::uint32_t height,
    int quality, int alpha_quality) {
  if (!width || !height) return awj::compat::unexpected{"渐进编码尺寸不能为 0。"};
  if (auto valid = validate_avif_layers(config, true, quality); !valid)
    return awj::compat::unexpected{valid.error()};
  if (!config.enabled()) return std::vector<AvifLayerPlan>{{width, height, quality, alpha_quality}};
  if (config.automatic)
    return std::vector<AvifLayerPlan>{{(width + 1) / 2, (height + 1) / 2, 10, alpha_quality},
                                     {width, height, quality, alpha_quality}};
  std::vector<AvifLayerPlan> plans;
  plans.reserve(*config.count);
  for (int i = 0; i < *config.count; ++i) {
    const auto& layer = config.layers[i];
    auto w = width, h = height;
    if (layer.size) { w = layer.size->first; h = layer.size->second; }
    else if (layer.scale) {
      const auto [numerator, denominator] = *layer.scale;
      if (!numerator || !denominator || numerator > denominator)
        return awj::compat::unexpected{"渐进层比例无效。"};
      w = static_cast<std::uint32_t>((static_cast<std::uint64_t>(width) * numerator + denominator - 1) / denominator);
      h = static_cast<std::uint32_t>((static_cast<std::uint64_t>(height) * numerator + denominator - 1) / denominator);
    }
    if (!w || !h || w > width || h > height ||
        (!plans.empty() && (w < plans.back().width || h < plans.back().height)))
      return awj::compat::unexpected{"渐进层尺寸必须非递减，且不超过最终尺寸。"};
    if (i == *config.count - 1 && (w != width || h != height))
      return awj::compat::unexpected{"最后一层必须保持请求的完整输出尺寸。"};
    plans.push_back({w, h, layer.quality.value_or(quality), layer.alpha_quality.value_or(alpha_quality)});
  }
  return plans;
}

} // namespace awj
