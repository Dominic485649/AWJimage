#include "studio_fonts.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "awj_studio.h"

import awj.core;

namespace awj::studio {

static int CALLBACK enum_font_family_proc(const LOGFONTW*, const TEXTMETRICW*, DWORD,
                                   LPARAM param) {
  *reinterpret_cast<bool*>(param) = true;
  return 0;
}

static int CALLBACK collect_font_family_proc(const LOGFONTW* font, const TEXTMETRICW*,
                                      DWORD, LPARAM param) {
  const std::wstring_view family{font->lfFaceName};
  if (!family.empty() && family.front() != L'@') {
    reinterpret_cast<std::unordered_set<std::wstring>*>(param)->emplace(family);
  }
  return 1;
}

static bool system_font_available(std::wstring_view family) noexcept {
  if (family.empty()) {
    return false;
  }
  HDC dc = GetDC(nullptr);
  if (dc == nullptr) {
    return false;
  }
  LOGFONTW query{};
  query.lfCharSet = DEFAULT_CHARSET;
  const auto length =
      std::min<std::size_t>(family.size(), std::size(query.lfFaceName) - 1);
  std::copy_n(family.begin(), length, query.lfFaceName);
  bool found = false;
  EnumFontFamiliesExW(dc, &query, enum_font_family_proc,
                      reinterpret_cast<LPARAM>(&found), 0);
  ReleaseDC(nullptr, dc);
  return found;
}

std::string select_system_ui_font_family() {
  const std::array<std::wstring_view, 8> candidates{
      L"鸿蒙黑体",       L"HarmonyOS Sans SC", L"Microsoft YaHei UI",
      L"Microsoft YaHei", L"微软雅黑",          L"Segoe UI",
      L"SimHei",         L"SimSun"};
  for (const auto family : candidates) {
    if (system_font_available(family)) {
      return awj::utf8_from_wide(family);
    }
  }
  return {};
}

void apply_system_ui_font(AwjStudio& app) {
  const auto family = select_system_ui_font_family();
  app.set_ui_font_family(to_shared(family));
}

void load_system_font_options(AwjStudio& app) {
  std::unordered_set<std::wstring> families;
  if (HDC dc = GetDC(nullptr); dc != nullptr) {
    LOGFONTW query{};
    query.lfCharSet = DEFAULT_CHARSET;
    EnumFontFamiliesExW(dc, &query, collect_font_family_proc,
                        reinterpret_cast<LPARAM>(&families), 0);
    ReleaseDC(nullptr, dc);
  }
  std::vector<std::string> sorted;
  sorted.reserve(families.size());
  for (const auto& family : families) sorted.push_back(awj::utf8_from_wide(family));
  std::ranges::sort(sorted);
  std::vector<ComboOption> options;
  options.reserve(sorted.size() + 1);
  options.push_back(ComboOption{.text = to_shared("系统默认字体"), .enabled = true});
  for (const auto& family : sorted) {
    options.push_back(ComboOption{.text = to_shared(family), .enabled = true});
  }
  app.set_ui_font_options(std::make_shared<slint::VectorModel<ComboOption>>(std::move(options)));
  const auto selected = shared_to_string(app.get_ui_font_family());
  const auto found = std::ranges::find(sorted, selected);
  if (found == sorted.end()) {
    app.set_ui_font_family({});
    app.set_ui_font_index(0);
  } else {
    app.set_ui_font_index(static_cast<int>(std::distance(sorted.begin(), found)) + 1);
  }
}

}  // namespace awj::studio
