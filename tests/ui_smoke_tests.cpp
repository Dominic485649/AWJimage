#include <slint.h>
#include <slint-platform.h>
#include <private/slint_tests_helpers.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "awj_studio_ui_smoke.h"
#include "focus_reentrancy.h"
#include "font_combo_hover.h"
#include "../src/ui/queue_model.h"
#include "../src/ui/deferred_model.h"
#include "../src/ui/slint_string_util.h"

namespace {

int fail(std::string_view message) {
  std::fwrite(message.data(), 1, message.size(), stderr);
  std::fputc('\n', stderr);
  return 1;
}

bool write_snapshot_ppm(const std::filesystem::path& path, const auto& snapshot) {
  std::ofstream file{path, std::ios::binary};
  file << "P6\n" << snapshot.width() << ' ' << snapshot.height() << "\n255\n";
  for (const auto& pixel : snapshot) {
    file.put(static_cast<char>(pixel.r));
    file.put(static_cast<char>(pixel.g));
    file.put(static_cast<char>(pixel.b));
  }
  return static_cast<bool>(file);
}

std::string ui_snapshot_font_family() {
#ifdef _WIN32
  wchar_t* raw_family = nullptr;
  std::size_t length = 0;
  if (_wdupenv_s(&raw_family, &length, L"AWJ_UI_TEST_FONT") != 0) return {};
  const std::unique_ptr<wchar_t, decltype(&std::free)> family{raw_family, &std::free};
  if (!family || !*family) return {};
  const int size = WideCharToMultiByte(CP_UTF8, 0, family.get(), -1, nullptr, 0, nullptr, nullptr);
  if (size <= 1) return {};
  std::string utf8(static_cast<std::size_t>(size), '\0');
  if (!WideCharToMultiByte(CP_UTF8, 0, family.get(), -1, utf8.data(), size, nullptr, nullptr)) return {};
  utf8.pop_back();
  return utf8;
#else
  const auto* family = std::getenv("AWJ_UI_TEST_FONT");
  return family ? family : "";
#endif
}

int verify_font_rendering_snapshots(const std::filesystem::path& directory, float scale) {
  if (scale != 1 && scale != 1.25f && scale != 1.5f && scale != 1.75f && scale != 2)
    return fail("字体截图缩放必须为 1、1.25、1.5、1.75 或 2");
  if (!std::filesystem::create_directory(directory)) return fail("字体截图目录必须为新目录");
  auto app = AwjStudio::create();
  const auto family = ui_snapshot_font_family();
  std::vector<ComboOption> fonts{{.text = "系统默认字体", .enabled = true}};
  if (!family.empty()) fonts.push_back(ComboOption{.text = slint::SharedString{family}, .enabled = true});
  app->set_ui_font_options(std::make_shared<slint::VectorModel<ComboOption>>(std::move(fonts)));
  app->set_ui_font_index(family.empty() ? 0 : 1);
  app->set_ui_font_family(slint::SharedString{family});
  app->set_current_version(AWJ_UI_VERSION);
  app->set_selected_page(1);
  app->set_theme_index(1);
  slint::select_bundled_translation("");
  app->window().window_handle().set_const_scale_factor(scale);
  const slint::PhysicalSize size{{static_cast<std::uint32_t>(std::lround(1171 * scale)),
                                  static_cast<std::uint32_t>(std::lround(800 * scale))}};
  bool saw_opengl = false;
  if (app->window().set_rendering_notifier([&](slint::RenderingState, slint::GraphicsAPI api) {
        saw_opengl = saw_opengl || api == slint::GraphicsAPI::NativeOpenGL;
      })) return fail("正式界面截图必须使用可核对的原生 GPU 渲染器");
  const auto click_navigation = [&](std::string_view id) {
    const auto elements = slint::testing::ElementHandle::find_by_element_id(app, id);
    if (elements.size() != 1) return false;
    const auto position = elements[0].absolute_position();
    const auto extent = elements[0].size();
    const slint::LogicalPosition point{{position.x + extent.width / 2, position.y + extent.height / 2}};
    app->window().dispatch_pointer_press_event(point, slint::PointerEventButton::Left);
    app->window().dispatch_pointer_release_event(point, slint::PointerEventButton::Left);
    return true;
  };
  std::optional<std::string> error;
  int theme = 1;
  int captured = 0;
  int frame_attempts = 0;
  bool size_requested = false;
  bool navigation_checked = false;
  auto changed_at = std::chrono::steady_clock::now();
  const auto finish = [&](std::string message) {
    if (!message.empty()) error = std::move(message);
    app->hide();
    slint::quit_event_loop();
  };
  slint::Timer frames;
  // Winit 在原生事件循环中恢复渲染表面，不能在 show 后直接假定首帧已经存在。
  frames.start(slint::TimerMode::Repeated, std::chrono::milliseconds{50}, [&] {
    try {
      if (!size_requested) {
        app->window().set_size(size);
        size_requested = true;
        changed_at = std::chrono::steady_clock::now();
        return;
      }
      const auto snapshot = app->window().take_snapshot();
      const auto actual_size = app->window().size();
      if (!snapshot || snapshot->width() != size.width || snapshot->height() != size.height ||
          actual_size.width != size.width || actual_size.height != size.height || !saw_opengl) {
        if (++frame_attempts < 60) return;
        std::fprintf(stderr,
            "font-rendering frame requested=%ux%u window=%ux%u snapshot=%ux%u opengl=%s\n",
            size.width, size.height, actual_size.width, actual_size.height,
            snapshot ? snapshot->width() : 0, snapshot ? snapshot->height() : 0,
            saw_opengl ? "yes" : "no");
        finish("原生窗口未生成请求尺寸的 GPU 帧");
        return;
      }
      frame_attempts = 0;
      if (std::fabs(app->window().scale_factor() - scale) > 0.001f) {
        finish("原生窗口没有使用指定的字体截图缩放比例");
        return;
      }
#ifdef _WIN32
      if (!app->window().win32_hwnd()) {
        finish("正式界面截图缺少真实 Win32 窗口");
        return;
      }
#endif
      if (!navigation_checked) {
        if (!click_navigation("AwjStudio::settings-nav") || app->get_selected_page() == 1 ||
            !click_navigation("AwjStudio::queue-nav") || app->get_selected_page() != 1 ||
            std::string_view{app->get_ui_font_family()} != family) {
          finish("原生界面的导航点击或字体一致性检查失败");
          return;
        }
        navigation_checked = true;
        changed_at = std::chrono::steady_clock::now();
        return;
      }
      if (std::chrono::steady_clock::now() - changed_at < std::chrono::milliseconds{250}) return;
      if (!write_snapshot_ppm(directory / std::format("queue-theme{}.ppm", theme), *snapshot)) {
        finish("无法写入正式界面原始 GPU 帧");
        return;
      }
      std::printf("font-rendering frame theme=%d size=%ux%u scale=%.3f api=NativeOpenGL\n",
                  theme, snapshot->width(), snapshot->height(), double(app->window().scale_factor()));
      ++captured;
      if (theme == 1) {
        theme = 2;
        app->set_theme_index(theme);
        changed_at = std::chrono::steady_clock::now();
      } else {
        finish({});
      }
    } catch (const std::exception& exception) {
      finish(exception.what());
    }
  });
  app->window().on_close_requested([&] {
    error = "正式界面截图窗口在完成前关闭";
    slint::quit_event_loop();
    return slint::CloseRequestResponse::HideWindow;
  });
  slint::Timer timeout;
  timeout.start(slint::TimerMode::SingleShot, std::chrono::seconds{30}, [&] {
    finish("等待正式界面原生 GPU 截图超时");
  });
  app->show();
  slint::run_event_loop();
  frames.stop();
  timeout.stop();
  if (error) return fail(*error);
  if (captured != 2 || !navigation_checked || !saw_opengl) return fail("正式界面截图没有完成全部检查");
  std::printf("font-rendering snapshots scale=%.2f family=%s navigation=pass api=NativeOpenGL\n",
              scale, family.c_str());
  return 0;
}

// 1.0.14 回归：Studio「开始转换」时队列重置循环崩在 strlen(nullptr)。
// slint::SharedString 同时有 operator=(const SharedString&) 与
// operator=(const char*)；`value = {}` 会选中后者并传入空指针。下面的断言锁定
// 这个前提，真正的清空必须走 awj::studio::clear_shared_string。
static_assert(std::is_assignable_v<slint::SharedString&, const char*>,
              "slint::SharedString 不再接受 const char*：请复核 "
              "clear_shared_string 的注释与本回归测试是否仍然必要");

// Slint 1.18 native file payloads must survive without UTF-8/newline serialization.
int verify_native_file_payload() {
  namespace fs = std::filesystem;
  std::vector<fs::path> expected{fs::path{u8"/tmp/中文 space.png"},
                                 fs::path{"/tmp/line\nbreak.png"}};
#ifndef _WIN32
  expected.emplace_back(std::string{"/tmp/native-"} + char(0xff) + ".png");
#else
  expected.emplace_back(L"C:\\图片\\image.png");
#endif
  slint::DataTransfer data;
  data.set_file_paths(expected);
  const auto paths = data.file_paths();
  if (!paths || *paths != expected || data.plain_text()) {
    return fail("native file payload lost path bytes or used a text protocol");
  }
  data.set_file_paths({});
  if (data.has_file_paths() || data.file_paths()) {
    return fail("cleared native file payload retained paths");
  }
  return 0;
}

int verify_runtime_images() {
  for (const auto* name : {"ui/logo-light.png", "ui/logo-dark.png", "tests/slint_runtime_images/pixel.jpg",
                           "tests/slint_runtime_images/pixel.svg"}) {
    const auto path = (std::filesystem::path{AWJ_UI_SOURCE_DIR} / name).u8string();
    const auto image = slint::Image::load_from_path(slint::SharedString{
        std::string_view{reinterpret_cast<const char*>(path.data()), path.size()}});
    if (!image.size().width || !image.size().height) {
      return fail(std::format("required Slint runtime image decoder failed: {}", name));
    }
  }
  return 0;
}

int verify_shared_string_clearing() {
  slint::SharedString text{"worker 日志行"};
  if (text.empty()) {
    return fail("shared string setup failed");
  }
  awj::studio::clear_shared_string(text);
  if (!text.empty() || text.size() != 0) {
    return fail("clear_shared_string left content behind");
  }
  // 清空后必须能安全地跨 Slint 边界转换与读取：1.0.14 正是死在这条路径上的
  // std::string_view(nullptr) -> strlen(nullptr)。
  const std::string_view view{text};
  if (!view.empty()) {
    return fail("cleared shared string is not empty across the Slint boundary");
  }
  if (const std::string copy = awj::studio::shared_to_string(text);
      !copy.empty()) {
    return fail("shared_to_string did not round-trip the cleared string");
  }
  // 重新赋值后再清空，保证这不是只对 default-constructed 实例成立的巧合。
  text = awj::studio::to_shared("第二行日志");
  if (text.empty()) {
    return fail("shared string re-assignment failed");
  }
  awj::studio::clear_shared_string(text);
  if (!text.empty() || !awj::studio::shared_to_string(text).empty()) {
    return fail("clear_shared_string did not reset a populated string");
  }
  return 0;
}

void send_key(const slint::ComponentHandle<AwjStudio>& app,
              std::u8string_view key) {
  const slint::SharedString text{key};
  app->window().dispatch_key_press_event(text);
  app->window().dispatch_key_release_event(text);
}

void send_text_key(const slint::ComponentHandle<AwjStudio>& app,
                   std::string_view key) {
  const slint::SharedString text{key};
  app->window().dispatch_key_press_event(text);
  app->window().dispatch_key_release_event(text);
}

std::optional<slint::testing::ElementHandle> find_one(
    const slint::ComponentHandle<AwjStudio>& app, std::string_view label,
    std::optional<slint::language::AccessibleRole> role = std::nullopt) {
  auto elements =
      slint::testing::ElementHandle::find_by_accessible_label(app, label);
  std::optional<slint::testing::ElementHandle> match;
  for (const auto& element : elements) {
    if (role && element.accessible_role() != role) {
      continue;
    }
    if (match) {
      return std::nullopt;
    }
    match = element;
  }
  return match;
}

int verify_template_token_layout(const slint::ComponentHandle<AwjStudio>& app,
                                 float window_width, bool english) {
  app->window().set_size(slint::LogicalSize({window_width, 560.0f}));
  const auto collision = find_one(app, english ? "On conflict" : "重复处理",
                                  slint::language::AccessibleRole::Combobox);
  if (!collision || collision->absolute_position().x + collision->size().width > window_width - 10) {
    return fail(std::format("collision dropdown overflows: window={} right={}", window_width, collision ? collision->absolute_position().x + collision->size().width : -1));
  }
  const std::array<std::string_view, 6> zh{"参数", "日期", "时间", "随机", "哈希", "SHA"};
  const std::array<std::string_view, 6> en{"Params", "Date", "Time", "Rand", "Hash", "SHA"};
  const auto& labels = english ? en : zh;
  std::vector<slint::testing::ElementHandle> buttons;
  buttons.reserve(labels.size());
  for (const auto label : labels) {
    auto button = find_one(app, label, slint::language::AccessibleRole::Button);
    if (!button) {
      return fail(std::format("template token button '{}' is missing", label));
    }
    buttons.push_back(*button);
  }

  const float expected_width = buttons.front().size().width;
  if (std::fabs(expected_width - (english ? 68.0f : 70.0f)) > 0.5f) {
    return fail(std::format("template token button width changed: {:.1f}px",
                            expected_width));
  }
  for (const auto& button : buttons) {
    if (std::fabs(button.size().width - expected_width) > 0.5f) {
      return fail("template token buttons are not equal width");
    }
  }

  const auto y = [&buttons](std::size_t index) {
    return buttons[index].absolute_position().y;
  };
  const auto x = [&buttons](std::size_t index) {
    return buttons[index].absolute_position().x;
  };
  for (std::size_t index = 1; index < buttons.size(); ++index) {
    if (std::fabs(y(index) - y(0)) > 0.5f || x(index) <= x(index - 1)) {
      return fail("template token buttons did not form one aligned row");
    }
  }
  const float sha_right = x(5) + buttons[5].size().width;
  const float collision_right = collision->absolute_position().x + collision->size().width;
  if (window_width == 747.0f && std::fabs(sha_right - collision_right) > 1.0f)
    return fail(std::format("SHA and collision dropdown are not aligned: {:.1f}px", sha_right - collision_right));
  if (sha_right > window_width - 10.0f)
    return fail("SHA button overflows the queue header");
  const auto format = find_one(app, english ? "Queue output format" : "队列输出格式",
                               slint::language::AccessibleRole::Combobox);
  const auto preset = find_one(app, english ? "Queue preset" : "队列预设",
                               slint::language::AccessibleRole::Combobox);
  if (!format || !preset || std::fabs(format->size().width - preset->size().width) > 0.5f ||
      std::fabs(preset->absolute_position().x + preset->size().width - sha_right) > 1.0f)
    return fail("fixed queue selectors do not share the SHA right edge");
  return 0;
}

int verify_menu_toolbar_layout(const slint::ComponentHandle<AwjStudio>& app) {
  using Role = slint::language::AccessibleRole;
  app->set_selected_page(0);
  app->set_parameter_preset_index(1);
  for (const int language : {0, 1}) {
    slint::select_bundled_translation(language ? "en" : "");
    app->set_language_index(language);
    std::array<float, 2> narrow_widths{};
    for (const float width : {747.0f, 1171.0f, 1800.0f}) {
      app->window().set_size(slint::LogicalSize({width, 560.0f}));
      const auto install = find_one(app, language ? "Install context menu" : "安装右键菜单", Role::Button);
      const auto remove = find_one(app, language ? "Remove context menu" : "移除右键菜单", Role::Button);
      const auto save = find_one(app, language ? "Save parameters" : "保存参数", Role::Button);
      const auto preset = find_one(app, language ? "Parameter preset" : "参数预设", Role::Combobox);
      const auto save_preset = find_one(app, language ? "Save preset" : "保存预设", Role::Button);
      const auto delete_preset = find_one(app, language ? "Delete preset" : "删除预设", Role::Button);
      const auto heading = find_one(app, language ? "Common parameters" : "常用参数", Role::Text);
      if (!install || !remove || save || !preset || !save_preset || !delete_preset || !heading)
        return fail("context menu toolbar control is missing");
      const std::array buttons{*install, *remove};
      for (std::size_t i = 0; i < buttons.size(); ++i) {
        if (width == 747.0f) narrow_widths[i] = buttons[i].size().width;
        else if (std::fabs(buttons[i].size().width - narrow_widths[i]) > 0.5f)
          return fail("context menu action button stretched with the window");
        if (buttons[i].size().width < 60.0f || buttons[i].size().width > 190.0f)
          return fail("context menu action button is not text-sized");
        if (i && (std::fabs(buttons[i].absolute_position().y - buttons[0].absolute_position().y) > 0.5f ||
                  buttons[i].absolute_position().x < buttons[i - 1].absolute_position().x + buttons[i - 1].size().width))
          return fail("context menu action buttons overlap or wrap");
      }
      const float action_right = remove->absolute_position().x + remove->size().width;
      const float preset_left = preset->absolute_position().x;
      const float preset_y = preset->absolute_position().y;
      const float action_y = install->absolute_position().y;
      if (width == 747.0f ? preset_y <= action_y + 40.0f :
          (std::fabs(preset_y - action_y) > 2.5f || preset_left <= action_right))
        return fail("context menu toolbar did not use the expected one/two-row layout");
      if (save_preset->absolute_position().x < preset_left + preset->size().width ||
          delete_preset->absolute_position().x < save_preset->absolute_position().x + save_preset->size().width ||
          std::fabs(delete_preset->absolute_position().y - preset_y) > 0.5f ||
          delete_preset->absolute_position().x + delete_preset->size().width > width - 10.0f ||
          heading->absolute_position().y < preset_y + preset->size().height)
        return fail("context menu preset controls overlap or precede common parameters");
      if (width != 747.0f) {
        const float menu_heading_y = heading->absolute_position().y;
        app->set_parameter_preset_index(0);
        const auto builtin_heading = find_one(app, language ? "Common parameters" : "常用参数", Role::Text);
        if (!builtin_heading ||
            std::fabs(builtin_heading->absolute_position().y - menu_heading_y) > 1.0f)
          return fail("built-in and menu presets have different top spacing");
        app->set_parameter_preset_index(1);
      }
    }
    app->set_parameter_preset_index(2);
    if (!find_one(app, language ? "Install context menu" : "安装右键菜单", Role::Button) ||
        !find_one(app, language ? "Remove context menu" : "移除右键菜单", Role::Button))
      return fail("user preset did not show context menu actions");
    app->set_shell_menu_injection_supported(false);
    if (find_one(app, language ? "Install context menu" : "安装右键菜单", Role::Button))
      return fail("Linux user preset showed unsupported injection action");
    app->set_shell_menu_injection_supported(true);
    app->set_parameter_preset_index(1);
  }
  slint::select_bundled_translation("");
  app->set_language_index(0);
  return 0;
}

std::shared_ptr<slint::VectorModel<ComboOption>> font_options() {
  std::vector<ComboOption> options;
  options.reserve(15);
  for (int index = 0; index < 15; ++index) {
    options.push_back(ComboOption{
        .text = slint::SharedString{std::format(
            "Smoke Font {:02} With A Deliberately Long Family Name", index)},
        .enabled = true});
  }
  return std::make_shared<slint::VectorModel<ComboOption>>(std::move(options));
}

std::shared_ptr<slint::VectorModel<TaskRow>> task_rows() {
  std::vector<TaskRow> rows;
  rows.push_back(TaskRow{
      .order = "1",
      .filename =
          "failed-image-with-a-very-long-name-that-must-stay-contained.webp",
      .folder = "C:\\smoke\\input\\a-very-long-folder-name",
      .size = "4.2 MiB",
      .output_size = "-",
      .status = "失败",
      .output = "failed.avif",
      .log = "完整错误：测试 worker 返回了可滚动的详细错误文本。",
      .warning = true,
      .locked = true,
      .state = 3,
      .input_path = "C:\\smoke\\input\\failed-image.webp",
      .output_path = "C:\\smoke\\output\\failed-image.avif",
      .encoder = "aom",
      .threads = "1",
      .stage_timings =
          "decode 0.010s · prepare 0.002s · encode 1.250s · write 0.004s"});
  rows.push_back(TaskRow{.order = "2",
                         .filename = "completed.png",
                         .folder = "C:\\smoke\\input",
                         .size = "1.0 MiB",
                         .output_size = "256.0 KiB",
                         .status = "完成",
                         .output = "completed.avif",
                         .log = {},
                         .warning = false,
                         .locked = true,
                         .state = 2,
                         .input_path = "C:\\smoke\\input\\completed.png",
                         .output_path = "C:\\smoke\\output\\completed.avif",
                         .encoder = "aom",
                         .threads = "1",
                         .stage_timings =
                             "decode 0.008s · prepare 0.001s · encode 0.900s · write 0.003s"});
  return std::make_shared<slint::VectorModel<TaskRow>>(std::move(rows));
}

std::shared_ptr<slint::VectorModel<UpdateHistoryRow>> update_history_rows() {
  std::vector<UpdateHistoryRow> rows;
  rows.push_back(UpdateHistoryRow{
      .version = "1.0.2",
      .channel = "prerelease",
      .published_at = "2026-08-10T00:00:00Z",
      .release_url = "https://github.com/Dominic485649/AWJimage/releases/tag/1.0.2",
      .changelog_zh_cn = "更新测试版",
      .changelog_en = "Update test build"});
  rows.push_back(UpdateHistoryRow{
      .version = "1.0.1",
      .channel = "stable",
      .published_at = "2026-08-09T00:00:00Z",
      .release_url = "https://github.com/Dominic485649/AWJimage/releases/tag/1.0.1",
      .changelog_zh_cn = "稳定版测试记录",
      .changelog_en = "Stable test release"});
  return std::make_shared<slint::VectorModel<UpdateHistoryRow>>(std::move(rows));
}

template<class T>
void settle_combo_popup(const slint::ComponentHandle<T>& app) {
  const auto popups = slint::testing::ElementHandle::find_by_element_id(app, "SoftComboBox::popup-container");
  for (const auto& popup : popups) (void)popup.size();
  // running 的 change handler 先注册 Timer，再触发开场动画并求值几何。
  slint::private_api::testing::mock_elapsed_time(1);
  slint::private_api::testing::mock_elapsed_time(60);
  for (const auto& popup : popups) (void)popup.size();
  slint::private_api::testing::mock_elapsed_time(300);
}

int verify_chroma_popup(const slint::ComponentHandle<AwjStudio>& app,
                        int format, bool menu, bool english) {
  using Role = slint::language::AccessibleRole;
  const auto set_chroma = [&](int index) {
    if (menu) app->set_menu_chroma_index(index); else app->set_chroma_index(index);
  };
  const auto chroma_index = [&] {
    return menu ? app->get_menu_chroma_index() : app->get_chroma_index();
  };
  const auto set_representation = [&](int index) {
    if (menu) app->set_menu_avif_color_representation_index(index);
    else app->set_avif_color_representation_index(index);
  };
  slint::cbindgen_private::slint_testing_use_native_popup(&app->window().window_handle(), false);
  for (const int representation : {0, 1, 2}) {
    if (format == 3 && representation != 0) continue;
    set_representation(0);
    set_chroma(4); // Valid AVIF 400, or a restored obsolete JPG 400 UI selection.
    set_representation(representation);
    slint::private_api::testing::mock_elapsed_time(1);
    const int rows = format == 3 ? 4 : representation == 2 ? 2 : 5;
    if (representation == 2 && chroma_index() != 0)
      return fail("forcing AVIF Identity did not normalize an unsupported chroma selection to auto");
    const auto combo = find_one(app, english ? "Chroma subsampling" : "色度采样", Role::Combobox);
    if (!combo) return fail("chroma combobox is missing from a supported format");
    combo->invoke_accessible_default_action();
    settle_combo_popup(app);
    if (slint::cbindgen_private::slint_testing_active_popup_count(&app->window().window_handle()) != 1)
      return fail("chroma popup did not expand");
    const bool has_400 = slint::testing::ElementHandle::find_by_accessible_label(app, "400").size() > 0;
    if (has_400 != (rows == 5) ||
        (rows == 2 && (slint::testing::ElementHandle::find_by_accessible_label(app, "422").size() > 0 ||
                       slint::testing::ElementHandle::find_by_accessible_label(app, "420").size() > 0)))
      return fail("unsupported chroma was still present in expanded popup items");
    int visible_popups = 0;
    for (const auto& popup : slint::testing::ElementHandle::find_by_element_id(app, "SoftComboBox::popup-container")) {
      if (popup.size().height < 1 || popup.size().width < 1) continue;
      ++visible_popups;
      if (std::fabs(popup.size().height - (rows * 36.0f + 8.0f)) > 1.0f)
        return fail("filtered chroma model retained an unsupported or blank popup row");
    }
    if (visible_popups != 1) return fail(std::format(
        "chroma popup row geometry is unavailable: format={} menu={} english={} representation={} rows={} visible={} matches={}",
        format, menu, english, representation, rows, visible_popups,
        slint::testing::ElementHandle::find_by_element_id(app, "SoftComboBox::popup-container").size()));
    send_key(app, slint::platform::key_codes::End);
    send_key(app, slint::platform::key_codes::Return);
    if (chroma_index() != rows - 1)
      return fail("filtered chroma dropdown changed persisted auto/444/422/420/400 values");
    combo->invoke_accessible_default_action();
    send_key(app, slint::platform::key_codes::Home);
    send_key(app, slint::platform::key_codes::Return);
    if (chroma_index() != 0) return fail("chroma Home selection did not retain stored auto=0");
  }
  set_representation(0);
  set_chroma(0);
  return 0;
}

int verify_parameter_matrix(const slint::ComponentHandle<AwjStudio>& app) {
  using Role = slint::language::AccessibleRole;
  app->window().set_size(slint::LogicalSize({1220.0f, 2000.0f}));
  for (const int language : {0, 1}) {
    if (!slint::select_bundled_translation(language ? "en" : "")) return fail("missing matrix translation");
    app->set_language_index(language);
    for (const int theme : {1, 2}) {
      app->set_theme_index(theme);
      for (const int page : {0, 1}) {
        app->set_selected_page(0);
        app->set_parameter_preset_index(page);
        for (int format = 0; format < 5; ++format) {
          if (page == 0) app->set_format_index(format);
          else app->set_menu_format_index(format);
          const auto check = [&](std::string_view zh, std::string_view en, Role role, bool expected) {
            const auto control = find_one(app, language ? en : zh, role);
            if (control.has_value() != expected) {
              return fail(std::format("parameter matrix: language={} theme={} page={} format={} control={} expected={}",
                                      language, theme, page, format, en, expected));
            }
            if (control && (control->size().width <= 0 || control->size().height <= 0 ||
                            control->absolute_position().x + control->size().width > 1221 ||
                            control->absolute_position().y + control->size().height > 2001))
              return fail(std::format("parameter control is clipped: {}", en));
            return 0;
          };
          if (check("质量", "Quality", Role::TextInput, true) ||
              check("AVIF 编码器", "AVIF encoder", Role::Combobox, false) ||
              check("动画 tune", "Animation tune", Role::Combobox, false) ||
              check("关键帧间隔", "Keyframe interval", Role::Text, false) ||
              check("视觉质量", "Visual quality", Role::TextInput, format != 4) ||
              check("速度", "Speed", Role::TextInput, format < 3) ||
              check("线程", "Threads", Role::TextInput, true) ||
              check("内存限制", "Memory limit", Role::TextInput, true) ||
              check("右键菜单选项", "Context menu options", Role::Text, page == 1) ||
              check("位深", "Bit depth", Role::TextInput, format == 0) ||
              check("色度采样", "Chroma subsampling", Role::Combobox, format == 0 || format == 3) ||
              check("JPG 渐进级别", "JPG progressive level", Role::Combobox, format == 3) ||
              check("启用 XYB", "Enable XYB", Role::Checkbox, format == 3)) return 1;
          if ((format == 0 || format == 3) && verify_chroma_popup(app, format, page == 1, language == 1)) return 1;
          if (theme == 1 && format == 0) {
            for (const float width : {1220.0f, 1800.0f}) {
              app->window().set_size(slint::LogicalSize({width, 2000.0f}));
              const auto common = find_one(app, language ? "Common parameters" : "常用参数", Role::Text);
              const auto format_label = find_one(app, language ? "Edit format" : "编辑格式", Role::Text);
              const auto format_control = find_one(app, language ? "Edit format" : "编辑格式", Role::Combobox);
              const auto resource = find_one(app, language ? "Resource limits" : "资源限制", Role::Text);
              const auto thread_label = find_one(app, language ? "Threads" : "线程", Role::Text);
              const auto thread_control = find_one(app, language ? "Threads" : "线程", Role::TextInput);
              const auto advanced = find_one(app, language ? "Advanced format options" : "格式高级选项", Role::Text);
              const auto depth_label = find_one(app, language ? "Bit depth" : "位深", Role::Text);
              const auto depth_control = find_one(app, language ? "Bit depth" : "位深", Role::TextInput);
              if (!common || !format_label || !format_control || !resource || !thread_label ||
                  !thread_control || !advanced || !depth_label || !depth_control)
                return fail("section alignment controls missing");
              const auto center_y = [](const auto& element) {
                return element->absolute_position().y + element->size().height / 2.0f;
              };
              if (std::fabs(center_y(format_label) - center_y(format_control)) > 1.0f ||
                  std::fabs(center_y(thread_label) - center_y(thread_control)) > 1.0f ||
                  std::fabs(center_y(depth_label) - center_y(depth_control)) > 1.0f ||
                  std::fabs(common->size().height - 30.0f) > 1.0f ||
                  std::fabs(resource->size().height - 30.0f) > 1.0f ||
                  std::fabs(advanced->size().height - 30.0f) > 1.0f ||
                  format_control->absolute_position().y < common->absolute_position().y + common->size().height + 9.0f ||
                  thread_control->absolute_position().y < resource->absolute_position().y + resource->size().height + 9.0f ||
                  depth_control->absolute_position().y < advanced->absolute_position().y + advanced->size().height + 9.0f)
                return fail("section first rows are not centered or spaced consistently");
            }
            app->window().set_size(slint::LogicalSize({1220.0f, 2000.0f}));
          }
          if (format == 3) {
            for (const int progressive : {0, 1, 2}) {
              if (page == 0) app->set_jpegli_progressive_index(progressive);
              else app->set_menu_jpegli_progressive_index(progressive);
              slint::private_api::testing::mock_elapsed_time(1);
              if (check("优化哈夫曼表", "Optimize Huffman tables", Role::Checkbox, progressive == 0)) return 1;
              if (progressive > 0 && !(page == 0 ? app->get_jpegli_optimize_huffman() : app->get_menu_jpegli_optimize_huffman()))
                return fail("progressive JPG did not force Huffman optimization");
            }
          }
        }
      }
    }
  }
  slint::select_bundled_translation("");
  app->set_language_index(0);
  app->set_format_index(0);
  app->set_menu_format_index(0);
  app->set_parameter_preset_index(0);
  return 0;
}

int verify_font_hover_stability() {
  auto app = awj_font_hover_test::FontComboHover::create();
  std::vector<awj_font_hover_test::ComboOption> options;
  for (int i = 0; i < 25; ++i)
    options.push_back({.text = slint::SharedString{std::format("Long Font {:02}", i)}, .enabled = true});
  app->set_options(std::make_shared<slint::VectorModel<awj_font_hover_test::ComboOption>>(std::move(options)));
  app->set_selected_index(8);
  app->show();
  slint::cbindgen_private::slint_testing_use_native_popup(&app->window().window_handle(), false);
  const auto combo = slint::testing::ElementHandle::find_by_element_id(app, "FontComboHover::combo");
  if (combo.size() != 1) return fail("font hover fixture combobox is missing");
  combo[0].invoke_accessible_default_action();
  if (!app->get_popup_open()) return fail("long font popup did not open");
  settle_combo_popup(app);
  const float initial_scroll = app->get_scroll_y();
  if (initial_scroll >= -1.0f) return fail("long font list did not initially scroll to selected item");
  // Enter the list, traverse rows, then return to the selected font repeatedly.
  // Hover must never change the viewport or the selected value.
  const auto position = combo[0].absolute_position();
  std::array<bool, 25> hovered{};
  for (int repeat = 0; repeat < 10; ++repeat) {
    for (int row : {0, 3, 7, 2, 5, 1, 8, 3}) {
      const float x = position.x + 60.0f;
      const float y = position.y + 40.0f + 8.0f + 4.0f + row * 36.0f + 18.0f;
      app->window().dispatch_pointer_move_event(slint::LogicalPosition({x, y}));
      slint::private_api::testing::mock_elapsed_time(20);
      if (const int index = app->get_hovered_index(); index >= 0 && index < 25) hovered[index] = true;
      if (!app->get_popup_open() || app->get_selected_index() != 8 ||
          std::fabs(app->get_scroll_y() - initial_scroll) > 0.5f)
        return fail("font popup scrolled, closed, or selected during mouse hover");
    }
  }
  if (!hovered[8] || std::count(hovered.begin(), hovered.end(), true) < 4)
    return fail("font hover test did not traverse the selected font and multiple rows");
  const float popup_x = position.x + 60.0f;
  const float popup_y = position.y + 40.0f + 8.0f + 4.0f + 18.0f;
  app->window().dispatch_pointer_scroll_event(slint::LogicalPosition({popup_x, popup_y}), 0.0f, 36.0f);
  // Flickable 的滚轮平滑滚动依赖动画时钟，读取结果前完成这一帧的动画。
  slint::private_api::testing::mock_elapsed_time(300);
  if (app->get_scroll_y() <= initial_scroll + 1.0f || app->get_selected_index() != 8)
    return fail(std::format("font popup mouse wheel did not scroll independently of selection: before={} after={} selected={}",
        initial_scroll, float(app->get_scroll_y()), app->get_selected_index()));
  const auto key = [&](std::u8string_view value) {
    const slint::SharedString text{value};
    app->window().dispatch_key_press_event(text);
    app->window().dispatch_key_release_event(text);
  };
  key(slint::platform::key_codes::Home);
  if (std::fabs(app->get_scroll_y()) > 0.5f || app->get_hovered_index() != 0)
    return fail("font Home did not scroll highlighted option into view");
  key(slint::platform::key_codes::End);
  if (app->get_hovered_index() != 24 || app->get_scroll_y() >= -500.0f)
    return fail("font End did not scroll to the last option");
  key(slint::platform::key_codes::Return);
  if (app->get_selected_index() != 24 || app->get_popup_open())
    return fail("font Enter did not commit keyboard selection");
  combo[0].invoke_accessible_default_action();
  settle_combo_popup(app);
  app->window().dispatch_pointer_move_event(slint::LogicalPosition({popup_x, popup_y}));
  app->window().dispatch_pointer_press_event(slint::LogicalPosition({popup_x, popup_y}), slint::PointerEventButton::Left);
  app->window().dispatch_pointer_release_event(slint::LogicalPosition({popup_x, popup_y}), slint::PointerEventButton::Left);
  if (app->get_selected_index() != 15 || app->get_popup_open())
    return fail("font popup click did not select the first visible row");
  combo[0].invoke_accessible_default_action();
  key(slint::platform::key_codes::Escape);
  return app->get_popup_open() ? fail("font Escape did not close popup") : 0;
}

int verify_manual_update(const slint::ComponentHandle<AwjStudio>& app) {
  using Role = slint::language::AccessibleRole;
  app->set_selected_page(2);
  int checks = 0;
  app->on_check_update_requested([&] { ++checks; });
  for (const int language : {0, 1}) {
    slint::select_bundled_translation(language ? "en" : "");
    app->set_language_index(language);
    for (const float width : {747.0f, 796.0f, 1171.0f, 1440.0f, 1834.0f}) {
      app->window().set_size(slint::LogicalSize({width, 2000.0f}));
      app->set_update_checking(false);
      const auto button = find_one(app, language ? "Check for updates" : "检查更新", Role::Button);
      if (!button || button->size().width < 95 || button->size().width > 145 ||
          button->absolute_position().x + button->size().width > width - 10)
        return fail("manual update button is missing, oversized, or clipped");
      {
        const auto checkbox = find_one(app, language ? "Show changelog only after updates" : "仅在更新后显示更新日志", Role::Checkbox);
        if (!checkbox || button->absolute_position().x < checkbox->absolute_position().x + checkbox->size().width ||
            std::fabs(button->absolute_position().y - checkbox->absolute_position().y) > 8.0f)
          return fail("manual update button is not directly right of the changelog checkbox");
      }
      const int previous = checks;
      button->invoke_accessible_default_action();
      if (checks != previous + 1) return fail("manual check callback was not dispatched");
      app->set_update_checking(true);
      button->invoke_accessible_default_action();
      if (checks != previous + 1) return fail("checking allowed a duplicate request");
    }
  }
  app->on_check_update_requested([] {});
  app->set_update_checking(false);
  slint::select_bundled_translation("");
  app->set_language_index(0);
  return 0;
}

int verify_queue_option_layout(const slint::ComponentHandle<AwjStudio>& app) {
  using Role = slint::language::AccessibleRole;
  const std::array<std::string_view, 6> zh{"移除元数据", "写入 CSV 报告", "写入运行日志", "保留创建时间", "保留修改时间", "保留访问时间"};
  const std::array<std::string_view, 6> en{"Strip metadata", "Write CSV report", "Write run log", "Preserve creation time", "Preserve modification time", "Preserve access time"};
  app->set_selected_page(1);
  for (const int language : {0, 1}) {
    slint::select_bundled_translation(language ? "en" : "");
    app->set_language_index(language);
    for (const float width : {747.0f, 796.0f, 960.0f, 1171.0f, 1440.0f}) {
      app->window().set_size(slint::LogicalSize({width, 827.0f}));
      const auto& labels = language ? en : zh;
      std::array<float, 6> tops{};
      float required_width = 50;
      float content_left = 0;
      float option_width = 0;
      for (std::size_t i = 0; i < labels.size(); ++i) {
        const auto option = find_one(app, labels[i], Role::Checkbox);
        if (!option) return fail("queue option is missing");
        if (i == 0) { content_left = option->absolute_position().x; option_width = option->size().width; }
        else if (std::fabs(option->size().width - option_width) > 0.5f) return fail("queue options are not equal width");
        tops[i] = option->absolute_position().y;
        required_width += option->size().width;
        if (option->size().width <= 20 ||
            option->absolute_position().x + option->size().width > width - 25.5f)
          return fail("queue option is clipped or has no text space");
      }
      const float available = width - 26 - content_left;
      const int columns = required_width <= available + 0.5f ? 6 : option_width * 3 + 20 <= available + 0.5f ? 3 : option_width * 2 + 10 <= available + 0.5f ? 2 : 1;
      for (std::size_t i = 0; i < tops.size(); ++i) {
        if (std::fabs(tops[i] - tops[(i / columns) * columns]) > 0.5f ||
            (i >= columns && tops[i] <= tops[i - columns]))
          return fail("queue options do not use the available equal-width columns");
      }
      const auto format = find_one(app, language ? "Queue output format" : "队列输出格式", Role::Combobox);
      const auto preset = find_one(app, language ? "Queue preset" : "队列预设", Role::Combobox);
      const auto collision = find_one(app, language ? "On conflict" : "重复处理", Role::Combobox);
      const auto sha = find_one(app, "SHA", Role::Button);
      const auto input = find_one(app, language ? "Input path" : "输入路径", Role::TextInput);
      const auto params = find_one(app, language ? "Params" : "参数", Role::Button);
      if (!format || !preset || !collision || !sha || !input || !params)
        return fail("queue alignment control is missing");
      const float edge = collision->absolute_position().x + collision->size().width;
      if (std::fabs(format->size().width - preset->size().width) > 0.5f ||
          std::fabs(preset->absolute_position().x + preset->size().width - sha->absolute_position().x - sha->size().width) > 0.5f ||
          (width == 747.0f && std::fabs(sha->absolute_position().x + sha->size().width - edge) > 0.5f) ||
          std::fabs(params->absolute_position().x - input->absolute_position().x) > 0.5f)
        return fail("queue controls do not share the path and collision edges");
    }
  }
  slint::select_bundled_translation("");
  app->set_language_index(0);
  return 0;
}

// Exercise pointer capture while each handle moves with its column edge.
int verify_queue_column_resize(const slint::ComponentHandle<AwjStudio>& app) {
  using Role = slint::language::AccessibleRole;
  app->set_selected_page(1);
  app->set_queue_failed_only(false);
  awj::ui::bind_queue_model(*app, task_rows());
  app->set_selected_queue_index(-1);
  app->set_queue_index_width(40.0f);
  app->set_queue_filename_width(170.0f);
  app->set_queue_dir_width(180.0f);
  app->set_queue_size_width(110.0f);
  app->set_queue_output_size_width(110.0f);
  app->set_queue_status_width(104.0f);
  app->set_queue_log_width(220.0f);
  app->window().set_size(slint::LogicalSize({1500.0f, 827.0f}));
  slint::select_bundled_translation("");

  const std::array<std::string_view, 7> titles{
      "#", "文件名", "目录", "源文件大小", "输出大小", "状态", "输出或日志"};
  const std::array<std::string_view, 7> values{
      "2", "completed.png", "C:\\smoke\\input", "1.0 MiB", "256.0 KiB", "完成", "completed.avif"};
  const auto widths = [&] {
    return std::array<float, 7>{app->get_queue_index_width(), app->get_queue_filename_width(),
        app->get_queue_dir_width(), app->get_queue_size_width(), app->get_queue_output_size_width(),
        app->get_queue_status_width(), app->get_queue_log_width()};
  };
  const auto visibility = [&] {
    return std::array<bool, 7>{app->get_queue_index_visible(), app->get_queue_filename_visible(),
        app->get_queue_dir_visible(), app->get_queue_size_visible(), app->get_queue_output_size_visible(),
        app->get_queue_status_visible(), app->get_queue_log_visible()};
  };
  // Slint 的无障碍查询不返回完全裁剪的单元格，先在宽窗保留句柄，再核对屏外几何。
  std::array<std::optional<slint::testing::ElementHandle>, 7> header_cells, row_cells;
  for (std::size_t i = 0; i < titles.size(); ++i) {
    header_cells[i] = find_one(app, titles[i], Role::Text);
    row_cells[i] = find_one(app, values[i], Role::Text);
  }
  const auto aligned = [&] {
    const auto sizes = widths();
    for (std::size_t i = 0; i < titles.size(); ++i) {
      const auto& header = header_cells[i];
      const auto& row = row_cells[i];
      if (!header || !row) {
        std::fprintf(stderr, "queue-column lookup index=%zu header=%s row=%s\n", i,
                     header ? "yes" : "no", row ? "yes" : "no");
        return false;
      }
      if (std::fabs(header->absolute_position().x - row->absolute_position().x) > 1 ||
          std::fabs(header->size().width - row->size().width) > 1 ||
          std::fabs(header->size().width - (sizes[i] - 20)) > 1) {
        std::fprintf(stderr, "queue-column geometry index=%zu header=(%.2f,%.2f) row=(%.2f,%.2f) expected=%.2f\n", i,
                     header->absolute_position().x, header->size().width,
                     row->absolute_position().x, row->size().width, sizes[i] - 20);
        return false;
      }
    }
    return true;
  };
  if (!aligned()) return fail("队列表头与数据行的固定列未对齐或缺少源/输出大小");
  const auto drag = [&](const auto& header, float delta) {
    const auto position = header->absolute_position();
    const float x = position.x + header->size().width + 9.0f;
    const float y = position.y + header->size().height / 2;
    app->window().dispatch_pointer_press_event(slint::LogicalPosition({x, y}), slint::PointerEventButton::Left);
    for (int step = 1; step <= 6; ++step)
      app->window().dispatch_pointer_move_event(slint::LogicalPosition({x + delta * step / 6, y}));
    app->window().dispatch_pointer_release_event(slint::LogicalPosition({x + delta, y}), slint::PointerEventButton::Left);
  };
  const std::array<float, 7> deltas{12, 60, 35, 24, 18, -18, 44};
  for (std::size_t i = 0; i < titles.size(); ++i) {
    const auto before = widths();
    const auto header = find_one(app, titles[i], Role::Text);
    if (!header) return fail("拖动前缺少队列表头");
    drag(header, deltas[i]);
    const auto after = widths();
    for (std::size_t j = 0; j < after.size(); ++j)
      if (std::fabs(after[j] - before[j] - (i == j ? deltas[i] : 0)) > 1)
        return fail(std::format("拖动列 {} 改变了非目标列 {} 或没有生效", i, j));
    if (!aligned()) return fail("拖动后表头与数据行错位");
  }
  const auto filename_before_minimum = app->get_queue_filename_width();
  const auto filename_for_minimum = find_one(app, "文件名", Role::Text);
  if (!filename_for_minimum) return fail("最小列宽检查缺少文件名表头");
  drag(filename_for_minimum, -1000);
  if (app->get_queue_filename_width() != 40.0f) return fail("列宽未限制为可操作的最小值");
  app->set_queue_filename_width(filename_before_minimum);
  const auto fixed_widths = widths();
  for (const float window_width : {1920.0f, 747.0f, 1100.0f, 820.0f}) {
    app->window().set_size(slint::LogicalSize({window_width, 827.0f}));
    slint::private_api::testing::mock_elapsed_time(300);
    const auto actual_widths = widths();
    if (actual_widths != fixed_widths) {
      for (std::size_t i = 0; i < actual_widths.size(); ++i)
        std::fprintf(stderr, "queue-column width window=%.0f index=%zu before=%.2f after=%.2f\n",
                     window_width, i, fixed_widths[i], actual_widths[i]);
      return fail("窗口缩放重新分配了队列列宽");
    }
    if (!aligned()) return fail(std::format("窗口宽度 {} 时表头与正文不对齐", window_width));
  }

  const auto lists = slint::testing::ElementHandle::find_by_element_id(app, "QueuePage::queue-list");
  const auto headers = slint::testing::ElementHandle::find_by_element_id(app, "QueuePage::queue-header");
  if (lists.size() != 1 || headers.size() != 1) return fail("队列滚动区或表头缺失");
  const auto list_pos = lists[0].absolute_position();
  const auto list_size = lists[0].size();
  const slint::LogicalPosition scrollbar_point{{list_pos.x + list_size.width / 2, list_pos.y + list_size.height - 6}};
  const auto filename_header = find_one(app, "文件名", Role::Text);
  if (!filename_header) return fail("横向滚动前缺少文件名表头");
  const auto before_scroll_x = filename_header->absolute_position().x;
  const auto header_y = headers[0].absolute_position().y;
  app->window().dispatch_pointer_scroll_event(scrollbar_point, -360, 0);
  slint::private_api::testing::mock_elapsed_time(300);
  if (filename_header->absolute_position().x >= before_scroll_x - 10 || !aligned() ||
      std::fabs(headers[0].absolute_position().y - header_y) > 0.5f || widths() != fixed_widths)
    return fail("横向滚动条没有同步移动表头/正文，或改变了列宽");

  const auto source_header = find_one(app, "源文件大小", Role::Text);
  if (!source_header) return fail("滚动后缺少源文件大小表头");
  drag(source_header, 30);
  auto resized_widths = fixed_widths;
  resized_widths[3] += 30;
  if (widths() != resized_widths || !aligned()) return fail("横向滚动后拖动没有只调整左列");
  app->window().dispatch_pointer_scroll_event(scrollbar_point, 100000, 0);
  slint::private_api::testing::mock_elapsed_time(300);
  const float source_x = source_header->absolute_position().x;
  app->set_queue_dir_visible(false);
  slint::private_api::testing::mock_elapsed_time(20);
  if (std::fabs(source_header->absolute_position().x - source_x + app->get_queue_dir_width()) > 1 ||
      widths() != resized_widths) return fail("隐藏列留下空隙或修改了列宽");
  app->set_selected_page(2);
  if (!slint::testing::ElementHandle::find_by_element_id(app, "QueuePage::queue-header").empty())
    return fail("切换页面没有真正移除队列组件");
  app->set_selected_page(1);
  if (app->get_queue_dir_visible() || widths() != resized_widths) return fail("切换页面丢失列宽或显隐状态");
  const auto restored_filename = find_one(app, "文件名", Role::Text);
  const auto restored_source = find_one(app, "源文件大小", Role::Text);
  const auto restored_row_source = find_one(app, "1.0 MiB", Role::Text);
  if (!restored_filename || !restored_source || !restored_row_source || find_one(app, "目录", Role::Text) ||
      std::fabs(restored_filename->size().width - (resized_widths[1] - 20)) > 1 ||
      std::fabs(restored_source->size().width - (resized_widths[3] - 20)) > 1 ||
      std::fabs(restored_source->absolute_position().x - restored_filename->absolute_position().x - resized_widths[1]) > 1 ||
      std::fabs(restored_source->absolute_position().x - restored_row_source->absolute_position().x) > 1)
    return fail("队列组件重建后没有恢复实际列几何与显隐绑定");
  app->set_queue_dir_visible(true);

  const auto open_menu = [&] {
    const auto header = slint::testing::ElementHandle::find_by_element_id(app, "QueuePage::queue-header");
    if (header.size() != 1) return false;
    const auto pos = header[0].absolute_position();
    const slint::LogicalPosition point{{pos.x + 16, pos.y + 16}};
    app->window().dispatch_pointer_press_event(point, slint::PointerEventButton::Right);
    app->window().dispatch_pointer_release_event(point, slint::PointerEventButton::Right);
    if (slint::cbindgen_private::slint_testing_active_popup_count(&app->window().window_handle()) != 1)
      return false;
    // 列菜单现在也有开场动画，先推进真实绑定和计时器，再验证完成后的键盘操作。
    const auto surfaces = slint::testing::ElementHandle::find_by_element_id(app, "AwjStudio::queue-column-menu-surface");
    for (const auto& surface : surfaces) (void)surface.size();
    slint::private_api::testing::mock_elapsed_time(1);
    slint::private_api::testing::mock_elapsed_time(25);
    for (const auto& surface : surfaces) (void)surface.size();
    slint::private_api::testing::mock_elapsed_time(250);
    return slint::cbindgen_private::slint_testing_active_popup_count(&app->window().window_handle()) == 1;
  };
  // 全部列均允许隐藏，空白表头仍能右键恢复；空队列也必须可以配置列。
  awj::ui::bind_queue_model(*app, std::make_shared<slint::VectorModel<TaskRow>>());
  for (const bool show : {false, true}) {
    if (!open_menu()) return fail("空队列或全隐藏表头无法打开列菜单");
    for (std::size_t i = 0; i < titles.size(); ++i) {
      send_key(app, slint::platform::key_codes::Space);
      const auto visible = visibility();
      for (std::size_t j = 0; j < visible.size(); ++j)
        if (visible[j] != (j <= i ? show : !show)) return fail("列菜单复选框没有切换对应列");
      if (slint::cbindgen_private::slint_testing_active_popup_count(&app->window().window_handle()) != 1)
        return fail("勾选列时菜单意外关闭");
      if (i + 1 < titles.size()) send_key(app, slint::platform::key_codes::Tab);
    }
    send_key(app, slint::platform::key_codes::Escape);
    if (slint::cbindgen_private::slint_testing_active_popup_count(&app->window().window_handle()) != 0)
      return fail("列菜单未响应 Escape");
    if (widths() != resized_widths) return fail("切换列显隐丢失调整后的宽度");
  }
  awj::ui::bind_queue_model(*app, task_rows());
  app->window().set_size(slint::LogicalSize({1500.0f, 827.0f}));
  for (const int theme : {1, 2}) {
    app->set_theme_index(theme);
    for (const int language : {0, 1}) {
      slint::select_bundled_translation(language ? "en" : "");
      app->set_language_index(language);
      const auto separators = slint::testing::ElementHandle::find_by_element_id(app, "ColumnResizer::separator");
      if (separators.size() != titles.size()) return fail("并非所有队列列都有拖动分隔线");
      const auto scale = app->window().scale_factor();
      for (const auto& separator : separators) {
        if (std::fabs(separator.size().width * scale - 1) > 0.01f)
          return fail("标题分隔线不是单个物理像素");
        const auto physical_x = separator.absolute_position().x * scale;
        if (std::fabs(physical_x - std::round(physical_x)) > 0.01f)
          return fail("标题分隔线没有对齐物理像素网格");
      }
      if (!find_one(app, language ? "Source size" : "源文件大小", Role::Text) ||
          !find_one(app, language ? "Output size" : "输出大小", Role::Text) || widths() != resized_widths)
        return fail("主题或语言切换丢失大小列或固定列宽");
    }
  }
  slint::select_bundled_translation("");
  app->set_language_index(0);
  return 0;
}

// 只覆盖表头细线的真实 GPU 像素，不加载控制器、用户配置或系统右键菜单。
int verify_queue_separator_snapshots(const std::filesystem::path& directory, float scale) {
  if (scale != 1 && scale != 1.25f && scale != 1.5f && scale != 2)
    return fail("分隔线截图缩放必须为 1、1.25、1.5 或 2");
  if (!std::filesystem::create_directory(directory)) return fail("分隔线截图目录必须为新目录");
  std::ofstream metrics{directory / "pixels.tsv"};
  metrics << "theme\tseparator\tscale\tx\ty\tlogical_x\tlogical_width\tline_r\tline_g\tline_b\tbg_r\tbg_g\tbg_b\n";
  if (!metrics) return fail("无法创建分隔线像素记录");
  bool saw_opengl = false;
  auto app = AwjStudio::create();
  app->set_current_version(AWJ_UI_VERSION);
  app->set_selected_page(1);
  app->set_selected_queue_index(-1);
  // 仅测试夹具缩窄目录列，让全部分隔线同时位于截图视口内；产品默认宽度不变。
  app->set_queue_dir_width(160);
  awj::ui::bind_queue_model(*app, std::make_shared<slint::VectorModel<TaskRow>>());
  slint::select_bundled_translation("");
  app->window().window_handle().set_const_scale_factor(scale);
  const slint::PhysicalSize size{{static_cast<std::uint32_t>(std::lround(1171 * scale)),
                                  static_cast<std::uint32_t>(std::lround(800 * scale))}};
  if (app->window().set_rendering_notifier([&](slint::RenderingState, slint::GraphicsAPI api) {
        saw_opengl = saw_opengl || api == slint::GraphicsAPI::NativeOpenGL;
      })) return fail("分隔线像素回归需要原生 OpenGL 渲染器");
  std::optional<std::string> error;
  int theme = 1, captures = 0, frame_attempts = 0;
  std::size_t checked_separators = 0;
  bool size_requested = false, theme_set = false;
  auto changed_at = std::chrono::steady_clock::now();
  const auto finish = [&](std::string message) {
    if (!message.empty()) error = std::move(message);
    app->hide();
    slint::quit_event_loop();
  };
  slint::Timer frames;
  frames.start(slint::TimerMode::Repeated, std::chrono::milliseconds{50}, [&] {
    try {
      if (!size_requested) {
        app->window().set_size(size);
        size_requested = true;
        return;
      }
      const auto snapshot = app->window().take_snapshot();
      const auto window_size = app->window().size();
      if (!snapshot || snapshot->width() != size.width || snapshot->height() != size.height ||
          window_size.width != size.width || window_size.height != size.height || !saw_opengl) {
        if (++frame_attempts < 80) return;
        finish("分隔线像素回归未获取请求尺寸的原生 GPU 帧");
        return;
      }
      frame_attempts = 0;
      if (std::fabs(app->window().scale_factor() - scale) > 0.001f) {
        finish("分隔线像素回归未使用指定缩放比例");
        return;
      }
#ifdef _WIN32
      if (!app->window().win32_hwnd()) { finish("分隔线像素回归没有真实 Win32 窗口"); return; }
#endif
      if (!theme_set) {
        app->set_theme_index(theme);
        theme_set = true;
        changed_at = std::chrono::steady_clock::now();
        return;
      }
      if (std::chrono::steady_clock::now() - changed_at < std::chrono::milliseconds{350}) return;
      const auto separators = slint::testing::ElementHandle::find_by_element_id(app, "ColumnResizer::separator");
      if (separators.size() != 7) { finish("原生截图没有包含全部队列分隔线"); return; }
      const auto line = theme == 2 ? std::array{105, 105, 105} : std::array{158, 172, 192};
      const auto background = theme == 2 ? std::array{48, 48, 48} : std::array{251, 253, 255};
      const auto pixel_matches = [&](int x, int y, const std::array<int, 3>& expected) {
        const auto& pixel = snapshot->begin()[static_cast<std::size_t>(y) * snapshot->width() + x];
        return std::abs(int(pixel.r) - expected[0]) <= 2 &&
               std::abs(int(pixel.g) - expected[1]) <= 2 &&
               std::abs(int(pixel.b) - expected[2]) <= 2;
      };
      for (std::size_t i = 0; i < separators.size(); ++i) {
        const auto position = separators[i].absolute_position();
        const auto extent = separators[i].size();
        const auto physical_x = position.x * scale;
        const int x = static_cast<int>(std::lround(physical_x));
        const int y = static_cast<int>(std::lround((position.y + extent.height / 2) * scale));
        if (std::fabs(extent.width * scale - 1) > 0.01f ||
            std::fabs(physical_x - std::round(physical_x)) > 0.01f ||
            x < 1 || x + 1 >= static_cast<int>(snapshot->width()) ||
            y < 0 || y >= static_cast<int>(snapshot->height()) ||
            !pixel_matches(x, y, line) || !pixel_matches(x - 1, y, background) ||
            !pixel_matches(x + 1, y, background)) {
          (void)write_snapshot_ppm(directory / std::format("failed-theme{}.ppm", theme), *snapshot);
          finish(std::format("分隔线像素错误：theme={} separator={} x={} y={} width={:.4f}",
              theme, i, x, y, extent.width * scale));
          return;
        }
        metrics << std::format("{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\n",
            theme, i, scale, x, y, position.x, extent.width, line[0], line[1], line[2],
            background[0], background[1], background[2]);
        ++checked_separators;
      }
      if (!write_snapshot_ppm(directory / std::format("theme{}.ppm", theme), *snapshot)) {
        finish("无法保存分隔线原生截图");
        return;
      }
      ++captures;
      if (theme == 2) { finish({}); return; }
      theme = 2;
      theme_set = false;
    } catch (const std::exception& exception) { finish(exception.what()); }
  });
  app->window().on_close_requested([&] {
    error = "分隔线回归窗口在完成前关闭";
    slint::quit_event_loop();
    return slint::CloseRequestResponse::HideWindow;
  });
  slint::Timer timeout;
  timeout.start(slint::TimerMode::SingleShot, std::chrono::seconds{20}, [&] { finish("分隔线像素回归超时"); });
  app->show();
  slint::run_event_loop();
  frames.stop();
  timeout.stop();
  metrics.flush();
  if (error) return fail(*error);
  if (captures != 2 || checked_separators != 14 || !saw_opengl || !metrics)
    return fail("分隔线像素回归未覆盖全部主题和列");
  std::printf("queue-separators scale=%.2f themes=%d separators=%zu physical_width=1 api=NativeOpenGL result=passed\n",
              double(scale), captures, checked_separators);
  return 0;
}

// 直接渲染生产组件，覆盖所有列的键盘焦点和勾选状态；不加载用户配置或同步系统菜单。
int verify_menu_rendering_snapshots(const std::filesystem::path& directory, float scale) {
  if (scale != 1 && scale != 1.25f && scale != 1.5f && scale != 2)
    return fail("菜单截图缩放必须为 1、1.25、1.5 或 2");
  if (!std::filesystem::create_directory(directory)) return fail("菜单截图目录必须为新目录");
  bool saw_opengl = false;
  auto app = AwjStudio::create();
  app->set_current_version(AWJ_UI_VERSION);
  app->set_selected_page(1);
  app->set_selected_queue_index(-1);
  awj::ui::bind_queue_model(*app, task_rows());
  slint::select_bundled_translation("");
  app->window().window_handle().set_const_scale_factor(scale);
  const slint::PhysicalSize size{{static_cast<std::uint32_t>(std::lround(1171 * scale)),
                                  static_cast<std::uint32_t>(std::lround(800 * scale))}};
  struct MenuCase { int theme; int column; bool checked; };
  std::vector<MenuCase> cases;
  for (const int theme : {1, 2}) {
    for (int column = 0; column < 7; ++column) {
      cases.push_back({theme, column, true});
      cases.push_back({theme, column, false});
    }
    cases.push_back({theme, -1, true});
  }
  constexpr float menu_x = 300, menu_y = 200;
  if (app->window().set_rendering_notifier([&](slint::RenderingState, slint::GraphicsAPI api) {
        saw_opengl = saw_opengl || api == slint::GraphicsAPI::NativeOpenGL;
      })) return fail("菜单像素回归需要原生 OpenGL 渲染器");
  std::optional<std::string> error;
  std::size_t case_index = 0;
  int captures = 0, frame_attempts = 0;
  bool size_requested = false, case_open = false, case_keyboard_sent = false;
  auto changed_at = std::chrono::steady_clock::now();
  const auto finish = [&](std::string message) {
    if (!message.empty()) error = std::move(message);
    app->hide();
    slint::quit_event_loop();
  };
  slint::Timer frames;
  frames.start(slint::TimerMode::Repeated, std::chrono::milliseconds{50}, [&] {
    try {
      if (!size_requested) {
        app->window().set_size(size);
        size_requested = true;
        return;
      }
      const auto snapshot = app->window().take_snapshot();
      const auto window_size = app->window().size();
      if (!snapshot || snapshot->width() != size.width || snapshot->height() != size.height ||
          window_size.width != size.width || window_size.height != size.height || !saw_opengl) {
        if (++frame_attempts < 80) return;
        finish("菜单像素回归未获取请求尺寸的原生 GPU 帧");
        return;
      }
      frame_attempts = 0;
      if (std::fabs(app->window().scale_factor() - scale) > 0.001f) {
        finish("菜单像素回归未使用指定缩放比例");
        return;
      }
#ifdef _WIN32
      if (!app->window().win32_hwnd()) { finish("菜单像素回归没有真实 Win32 窗口"); return; }
#endif
      const auto current = cases.at(case_index);
      if (!case_open) {
        app->set_theme_index(current.theme);
        if (current.column >= 0) app->invoke_open_queue_column_menu(menu_x, menu_y);
        else app->invoke_open_queue_menu(0, menu_x, menu_y);
        case_open = true;
        case_keyboard_sent = false;
        changed_at = std::chrono::steady_clock::now();
        return;
      }
      if (std::chrono::steady_clock::now() - changed_at < std::chrono::milliseconds{350}) return;
      if (current.column >= 0 && !case_keyboard_sent) {
        // 开场完成后再操作，随后等待新增的焦点底色过渡，避免采到动画中的颜色。
        for (int i = 0; i < current.column; ++i) send_key(app, slint::platform::key_codes::Tab);
        if (!current.checked) send_key(app, slint::platform::key_codes::Space);
        const std::array<bool, 7> checked{app->get_queue_index_visible(), app->get_queue_filename_visible(),
            app->get_queue_dir_visible(), app->get_queue_size_visible(), app->get_queue_output_size_visible(),
            app->get_queue_status_visible(), app->get_queue_log_visible()};
        if (checked[static_cast<std::size_t>(current.column)] != current.checked) {
          finish("原生列菜单未响应对应选项的键盘切换");
          return;
        }
        case_keyboard_sent = true;
        changed_at = std::chrono::steady_clock::now();
        return;
      }
      const auto pixel_matches = [&](float x, float y, const std::array<int, 3>& expected) {
        const int px = static_cast<int>(std::floor((menu_x + x) * scale));
        const int py = static_cast<int>(std::floor((menu_y + y) * scale));
        const auto& pixel = snapshot->begin()[static_cast<std::size_t>(py) * snapshot->width() + px];
        return std::abs(int(pixel.r) - expected[0]) <= 8 &&
               std::abs(int(pixel.g) - expected[1]) <= 8 &&
               std::abs(int(pixel.b) - expected[2]) <= 8;
      };
      const std::array<int, 3> panel = current.theme == 2 ? std::array{40, 40, 40} : std::array{255, 255, 255};
      const auto failed_frame = [&](std::string message) {
        (void)write_snapshot_ppm(directory / std::format("failed-theme{}-column{}-checked{}.ppm",
            current.theme, current.column, current.checked), *snapshot);
        finish(std::move(message));
      };
      if (!pixel_matches(3, 80, panel) || !pixel_matches(3, 160, panel) || !pixel_matches(3, 250, panel)) {
        failed_frame(std::format("菜单背景像素未跟随主题：theme={} column={}", current.theme, current.column));
        return;
      }
      if (current.column >= 0) {
        const std::array<int, 3> focus = current.theme == 2 ? std::array{20, 50, 91} : std::array{231, 241, 255};
        const float center_y = 34 + 34 * static_cast<float>(current.column) + 16;
        // 取整行最右侧的内部像素，避开标签和勾选框；标准 FocusBorder 在这里会产生白/黑边。
        if (!pixel_matches(210.5f, center_y, focus) || !pixel_matches(209, center_y, focus)) {
          failed_frame(std::format("列菜单仍有外框或焦点底色错误：theme={} column={} checked={}",
              current.theme, current.column, current.checked));
          return;
        }
      }
      if (current.column == -1 || (current.column == 0 && current.checked) ||
          (current.column == 4 && !current.checked)) {
        const auto name = current.column == -1 ? std::format("tasks-theme{}.ppm", current.theme)
            : std::format("columns-theme{}-column{}.ppm", current.theme, current.column);
        if (!write_snapshot_ppm(directory / name, *snapshot)) { finish("无法保存菜单原生截图"); return; }
        ++captures;
      }
      if (current.column >= 0 && !current.checked) send_key(app, slint::platform::key_codes::Space);
      send_key(app, slint::platform::key_codes::Escape);
      case_open = false;
      if (++case_index == cases.size()) finish({});
    } catch (const std::exception& exception) { finish(exception.what()); }
  });
  app->window().on_close_requested([&] {
    error = "菜单回归窗口在完成前关闭";
    slint::quit_event_loop();
    return slint::CloseRequestResponse::HideWindow;
  });
  slint::Timer timeout;
  timeout.start(slint::TimerMode::SingleShot, std::chrono::seconds{45}, [&] { finish("菜单像素回归超时"); });
  app->show();
  slint::run_event_loop();
  frames.stop();
  timeout.stop();
  if (error) return fail(*error);
  if (case_index != cases.size() || captures != 6 || !saw_opengl) return fail("菜单像素回归未覆盖全部状态");
  std::printf("menu-rendering scale=%.2f cases=%zu snapshots=%d api=NativeOpenGL result=passed\n",
              double(scale), case_index, captures);
  return 0;
}

int run_scale(const slint::ComponentHandle<AwjStudio>& app,
              float scale_factor) {
  {
    int loads = 0;
    awj::ui::DeferredModel<int> deferred;
    deferred.set_loader([&loads] { ++loads; return std::vector<int>{1, 2}; });
    if (loads != 0 || deferred.row_count() != 2 || loads != 1 || deferred.row_data(1) != 2) {
      return fail("deferred history was loaded eagerly or returned the wrong row");
    }
    deferred.set_loader([&loads] { ++loads; return std::vector<int>{3}; });
    if (loads != 1 || deferred.row_data(0) != 3 || loads != 2) {
      return fail("deferred history did not invalidate on a verified update");
    }
  }
  app->window().window_handle().set_const_scale_factor(scale_factor);
  if (std::fabs(app->window().scale_factor() - scale_factor) > 0.001f)
    return fail("UI 回归没有使用请求的实际缩放比例");
  app->window().set_size(slint::LogicalSize({820.0f, 560.0f}));
  app->set_ui_font_options(font_options());
  awj::ui::bind_queue_model(*app, task_rows());
  app->set_update_history(update_history_rows());
  app->set_queue_failed_count(1);
  app->set_queue_success_count(1);
  app->set_queue_failed_only(false);
  app->set_selected_queue_index(0);
  app->set_current_version("1.0.1");
  app->set_update_available(true);
  app->set_update_version("1.0.2");
  app->set_update_published_at("2026-08-10T00:00:00Z");
  app->set_update_changelog_zh_cn("更新测试版");
  app->set_update_changelog_en("Update test build");
  app->set_update_summary_zh_cn("更新测试版");
  app->set_update_summary_en("Update test build");
  app->set_show_update_changelog(true);
  int retries = 0;
  int version_clicks = 0;
  app->on_retry_failed([&retries] { ++retries; });
  app->on_version_clicked([&version_clicks] { ++version_clicks; });
  const std::array pages{
      std::pair{"编码队列", 1}, std::pair{"参数设置", 0},
      std::pair{"更新日志", 4}, std::pair{"设置", 2}};
  for (const auto& [label, page] : pages) {
    auto element =
        find_one(app, label, slint::language::AccessibleRole::Tab);
    if (!element || element->accessible_role() !=
                        slint::language::AccessibleRole::Tab) {
      const auto window_size = app->window().size();
      const auto matches =
          slint::testing::ElementHandle::find_by_accessible_label(app, label);
      std::fprintf(stderr,
                   "navigation diagnostic: scale=%.1f window=%ux%u label=%s "
                   "matches=%zu\n",
                   static_cast<double>(scale_factor), window_size.width,
                   window_size.height, label, matches.size());
      for (const auto& match : matches) {
        const auto position = match.absolute_position();
        const auto size = match.size();
        std::fprintf(stderr,
                     "  role=%d pos=(%.1f,%.1f) size=(%.1f,%.1f)\n",
                     match.accessible_role()
                         ? static_cast<int>(*match.accessible_role())
                         : -1,
                     static_cast<double>(position.x),
                     static_cast<double>(position.y),
                     static_cast<double>(size.width),
                     static_cast<double>(size.height));
      }
      return fail("navigation item is missing its tab role or name");
    }
    element->invoke_accessible_default_action();
    if (app->get_selected_page() != page) {
      return fail("navigation accessibility action did not switch page");
    }
  }

  app->set_selected_page(1);
  send_key(app, slint::platform::key_codes::Tab);
  send_key(app, slint::platform::key_codes::Return);
  send_key(app, slint::platform::key_codes::DownArrow);
  send_key(app, slint::platform::key_codes::Return);
  if (app->get_selected_page() != 0) {
    return fail("Tab/Down/Enter navigation failed");
  }
  send_key(app, slint::platform::key_codes::End);
  send_key(app, slint::platform::key_codes::Return);
  if (app->get_selected_page() != 2) {
    return fail("End navigation failed");
  }
  send_key(app, slint::platform::key_codes::UpArrow);
  send_text_key(app, " ");
  if (app->get_selected_page() != 4) {
    return fail("Up/Space navigation did not reach the changelog page");
  }
  send_key(app, slint::platform::key_codes::UpArrow);
  send_text_key(app, " ");
  if (app->get_selected_page() != 0) {
    return fail("second Up/Space navigation did not reach parameters");
  }
  send_key(app, slint::platform::key_codes::Home);
  send_key(app, slint::platform::key_codes::Return);
  if (app->get_selected_page() != 1) {
    return fail("Home navigation failed");
  }

  app->set_selected_page(0);
  if (find_one(app, "菜单参数", slint::language::AccessibleRole::Tab))
    return fail("menu parameters still occupy a separate navigation page");
  if (find_one(app, "编码参数", slint::language::AccessibleRole::Button) ||
      find_one(app, "菜单参数", slint::language::AccessibleRole::Button))
    return fail("legacy parameter mode buttons are still present");
  auto parameter_options = std::make_shared<slint::VectorModel<ComboOption>>(
      std::vector<ComboOption>{{.text = "内置默认", .enabled = true},
                               {.text = "右键菜单", .enabled = true},
                               {.text = "测试用户预设", .enabled = true}});
  app->set_parameter_preset_options(parameter_options);
  auto queue_options = std::make_shared<slint::VectorModel<ComboOption>>(
      std::vector<ComboOption>{{.text = "内置默认", .enabled = true},
                               {.text = "测试用户预设", .enabled = true}});
  app->set_queue_preset_options(queue_options);
  if (app->get_parameter_preset_options()->row_count() != 3 ||
      std::string_view{app->get_parameter_preset_options()->row_data(0)->text} != "内置默认" ||
      std::string_view{app->get_parameter_preset_options()->row_data(1)->text} != "右键菜单" ||
      std::string_view{app->get_parameter_preset_options()->row_data(2)->text} != "测试用户预设" ||
      app->get_queue_preset_options()->row_count() != 2)
    return fail("parameter and queue preset option order is wrong");
  app->on_parameter_preset_selected([&](int index) { app->set_parameter_preset_index(index); });
  int installs = 0;
  int removals = 0;
  app->on_install_context_menu_requested([&] { ++installs; });
  app->on_remove_context_menu_requested([&] { ++removals; });
  const auto builtin_install = find_one(app, "安装右键菜单", slint::language::AccessibleRole::Button);
  const auto builtin_remove = find_one(app, "移除右键菜单", slint::language::AccessibleRole::Button);
  if (!find_one(app, "参数预设", slint::language::AccessibleRole::Combobox) ||
      !find_one(app, "保存预设", slint::language::AccessibleRole::Button) ||
      !builtin_install || !builtin_remove)
    return fail("built-in preset page is wrong");
  builtin_install->invoke_accessible_default_action();
  builtin_remove->invoke_accessible_default_action();
  if (installs != 0 || removals != 0)
    return fail("built-in preset menu actions must be disabled");
  app->set_preset_editor_open(true);
  app->set_preset_editor_existing(false);
  app->set_preset_editor_special_menu(false);
  if (!find_one(app, "预设名称", slint::language::AccessibleRole::TextInput) ||
      find_one(app, "删除", slint::language::AccessibleRole::Button))
    return fail("built-in save-as editor is wrong");
  app->set_preset_editor_open(false);
  const auto original_quality = app->get_quality_text();
  const auto original_menu_quality = app->get_menu_quality_text();
  app->set_quality_text("73");
  app->set_menu_quality_text("61");
  app->invoke_parameter_preset_selected(1);
  if (app->get_selected_page() != 0 ||
      !find_one(app, "参数预设", slint::language::AccessibleRole::Combobox) ||
      !find_one(app, "安装右键菜单", slint::language::AccessibleRole::Button))
    return fail("context menu preset did not show menu controls");
  if (const int result = verify_menu_toolbar_layout(app); result != 0) return result;
  app->set_preset_editor_open(true);
  app->set_preset_editor_existing(true);
  app->set_preset_editor_special_menu(true);
  app->set_preset_editor_name("右键菜单");
  app->set_preset_editor_description("测试简介");
  app->window().set_size(slint::LogicalSize({1220.0f, 1000.0f}));
  if (!find_one(app, "预设名称", slint::language::AccessibleRole::TextInput) ||
      !find_one(app, "预设简介", slint::language::AccessibleRole::TextInput) ||
      find_one(app, "删除", slint::language::AccessibleRole::Button))
    return fail("context menu preset editor is wrong");
  const auto panels = slint::testing::ElementHandle::find_by_element_id(app, "PresetEditor::preset-editor-panel");
  const auto editor_save = find_one(app, "保存", slint::language::AccessibleRole::Button);
  if (panels.size() != 1 || !editor_save) return fail("preset editor panel geometry is unavailable");
  const float bottom_gap = panels[0].absolute_position().y + panels[0].size().height -
                           editor_save->absolute_position().y - editor_save->size().height;
  if (bottom_gap < 10.0f || bottom_gap > 32.0f)
    return fail(std::format("preset editor retained {:.1f}px below its buttons", bottom_gap));
  app->set_preset_editor_open(false);
  int preset_saves = 0;
  app->on_open_preset_editor([&] { ++preset_saves; });
  find_one(app, "安装右键菜单", slint::language::AccessibleRole::Button)->invoke_accessible_default_action();
  find_one(app, "移除右键菜单", slint::language::AccessibleRole::Button)->invoke_accessible_default_action();
  find_one(app, "保存预设", slint::language::AccessibleRole::Button)->invoke_accessible_default_action();
  if (installs != 1 || removals != 1 || preset_saves != 1)
    return fail("menu mode lost an installation or save action");
  int repairs = 0;
  app->on_context_menu_warning_clicked([&] { ++repairs; });
  app->set_context_menu_warning("右键菜单版本不兼容，请点击修复。");
  const auto repair = find_one(app, "右键菜单版本不兼容，请点击修复。",
                               slint::language::AccessibleRole::Button);
  if (!repair || find_one(app, "保存参数", slint::language::AccessibleRole::Button))
    return fail("incompatible menu warning did not replace the removed action slot");
  const auto remove_action = find_one(app, "移除右键菜单", slint::language::AccessibleRole::Button);
  if (!remove_action ||
      std::fabs(repair->absolute_position().y - remove_action->absolute_position().y) > 0.5f ||
      repair->absolute_position().x < remove_action->absolute_position().x + remove_action->size().width)
    return fail("incompatible menu warning did not occupy the removed action position");
  repair->invoke_accessible_default_action();
  if (repairs != 1) return fail("incompatible menu warning lost its repair callback");
  app->set_running(true);
  find_one(app, "保存预设", slint::language::AccessibleRole::Button)->invoke_accessible_default_action();
  repair->invoke_accessible_default_action();
  app->set_running(false);
  if (preset_saves != 1 || repairs != 1) return fail("menu controls stayed active during a run");
  app->set_context_menu_warning({});
  app->invoke_parameter_preset_selected(2);
  if (app->get_selected_page() != 0 ||
      !find_one(app, "参数预设", slint::language::AccessibleRole::Combobox) ||
      !find_one(app, "安装右键菜单", slint::language::AccessibleRole::Button) ||
      app->get_quality_text() != "73" || app->get_menu_quality_text() != "61")
    return fail("switching presets mixed normal and menu parameter controls");
  find_one(app, "安装右键菜单", slint::language::AccessibleRole::Button)->invoke_accessible_default_action();
  find_one(app, "移除右键菜单", slint::language::AccessibleRole::Button)->invoke_accessible_default_action();
  if (installs != 2 || removals != 2)
    return fail("user preset context menu actions are not wired");
  app->invoke_parameter_preset_selected(0);
  app->set_quality_text(original_quality);
  app->set_menu_quality_text(original_menu_quality);

  auto settings =
      find_one(app, "设置", slint::language::AccessibleRole::Tab);
  settings->invoke_accessible_default_action();
  // A tall window must leave its spare space below the settings cards.
  app->window().set_size(slint::LogicalSize({1220.0f, 2000.0f}));
  auto theme_control = find_one(app, "主题", slint::language::AccessibleRole::Combobox);
  auto font_control = find_one(app, "字体", slint::language::AccessibleRole::Combobox);
  auto show_log = find_one(app, "显示更新日志", slint::language::AccessibleRole::Checkbox);
  auto only_updates = find_one(app, "仅在更新后显示更新日志", slint::language::AccessibleRole::Checkbox);
  auto check_updates = find_one(app, "检查更新", slint::language::AccessibleRole::Button);
  auto github = find_one(app, "https://github.com/Dominic485649/AWJimage", slint::language::AccessibleRole::Button);
  auto qq = find_one(app, "https://qm.qq.com/q/Wt4Ssh1igi", slint::language::AccessibleRole::Button);
  if (!theme_control || !font_control || !show_log || !only_updates || !check_updates || !github || !qq)
    return fail("settings controls or accessible links are missing");
  if (find_one(app, "AWJ的GitHub仓库地址：", slint::language::AccessibleRole::Button) ||
      find_one(app, "问题反馈/Q群交流：", slint::language::AccessibleRole::Button))
    return fail("About descriptions must not be clickable links");
  if (theme_control->absolute_position().y > 120 || font_control->absolute_position().y > 240 ||
      qq->absolute_position().y - github->absolute_position().y > 60)
    return fail("settings cards stretched their internal spacing in a tall window");
  const auto center_y = [](const auto& item) { return item->absolute_position().y + item->size().height / 2; };
  if (std::fabs(center_y(show_log) - center_y(only_updates)) > 2 ||
      std::fabs(center_y(show_log) - center_y(check_updates)) > 2)
    return fail("update controls are not on the same row");
  app->window().set_size(slint::LogicalSize({820.0f, 560.0f}));
  int menu_toggles = 0;
  app->on_shell_menu_compatibility_requested([&] { ++menu_toggles; });
  app->set_shell_menu_compatibility(false);
  auto compatibility = find_one(app, "兼容性右键菜单", slint::language::AccessibleRole::Checkbox);
  if (!compatibility) return fail("compatibility checkbox has no standalone label");
  compatibility->invoke_accessible_default_action();
  if (!app->get_shell_menu_compatibility() || menu_toggles != 1)
    return fail("compatibility checkbox could not be enabled");
  compatibility->invoke_accessible_default_action();
  if (app->get_shell_menu_compatibility() || menu_toggles != 2)
    return fail("compatibility checkbox could not be disabled");
  app->on_shell_menu_compatibility_requested([] {});
  const auto elevation_notice = "正在应用右键菜单设置；修改机器菜单时需要管理员权限。";
  app->set_menu_operation_active(true);
  app->set_menu_elevation_required(false);
  if (find_one(app, elevation_notice)) return fail("ordinary menu operation showed elevation overlay");
  app->set_menu_elevation_required(true);
  if (!find_one(app, elevation_notice)) return fail("elevated menu operation lost its notice");
  app->set_menu_operation_active(false);
  app->set_menu_elevation_required(false);
  if (find_one(app, elevation_notice)) return fail("completed menu operation retained its notice");
  auto font =
      find_one(app, "字体", slint::language::AccessibleRole::Combobox);
  if (!font || font->accessible_role() !=
                   slint::language::AccessibleRole::Combobox) {
    return fail("font combobox is missing its role or name");
  }
  slint::cbindgen_private::slint_testing_use_native_popup(
      &app->window().window_handle(), false);
  font->invoke_accessible_default_action();
  if (slint::cbindgen_private::slint_testing_active_popup_count(
          &app->window().window_handle()) != 1) {
    return fail("font popup did not open");
  }
  send_key(app, slint::platform::key_codes::End);
  send_key(app, slint::platform::key_codes::Return);
  if (app->get_ui_font_index() != 14) {
    return fail("font End selection did not reach the last item");
  }
  font->invoke_accessible_default_action();
  send_key(app, slint::platform::key_codes::Home);
  send_key(app, slint::platform::key_codes::Return);
  if (app->get_ui_font_index() != 0) {
    return fail("font Home selection did not reach the first item");
  }
  font->invoke_accessible_default_action();
  send_key(app, slint::platform::key_codes::Escape);
  if (slint::cbindgen_private::slint_testing_active_popup_count(
          &app->window().window_handle()) != 0) {
    return fail("font Escape did not close the popup");
  }
  const auto font_position = font->absolute_position();
  const auto font_size = font->size();
  app->window().dispatch_pointer_scroll_event(
      slint::LogicalPosition(
          {font_position.x + font_size.width / 2.0f,
           font_position.y + font_size.height / 2.0f}),
      0.0f, -36.0f);
  if (app->get_ui_font_index() != 1) {
    return fail(std::format(
        "focused font combobox did not handle the mouse wheel: scale={:.1f} "
        "index={} pos=({:.1f},{:.1f}) size=({:.1f},{:.1f})",
        scale_factor, app->get_ui_font_index(), font_position.x,
        font_position.y, font_size.width, font_size.height));
  }

  app->set_theme_index(1);
  if (app->get_theme_index() != 1) {
    return fail("light theme did not apply");
  }
  app->set_theme_index(2);
  if (app->get_theme_index() != 2) {
    return fail("dark theme did not apply");
  }

  app->set_selected_page(4);
  app->set_show_update_changelog(false);
  if (find_one(app, "更新日志", slint::language::AccessibleRole::Tab)) {
    return fail("hidden changelog remains in the accessible navigation tree");
  }
  auto version = find_one(app, "当前版本 1.0.1",
                          slint::language::AccessibleRole::Button);
  if (!version) {
    return fail("hiding the changelog also hid the version/update control");
  }
  version->invoke_accessible_default_action();
  if (version_clicks != 1) {
    return fail("version accessibility action did not fire");
  }
  app->set_selected_page(2);
  app->set_show_update_changelog(true);

  // 界面语言切换。中文是 @tr() 的 msgid 原文（语言索引 0），English 来自
  // ui/translations/en/LC_MESSAGES/awj.po 的 bundled 翻译。
  // select_bundled_translation 写 translations_dirty 这个真实属性，所有 @tr()
  // 绑定都会重算——可访问名也是 @tr() 的，所以这里直接用它来证明切换生效，
  // 而不是只检查那个 int 属性被写进去了。
  app->set_selected_page(0);
  if (!slint::select_bundled_translation("")) {
    return fail("could not select the Chinese default translation");
  }
  app->set_language_index(0);
  if (!find_one(app, "编辑格式", slint::language::AccessibleRole::Combobox)) {
    return fail("Chinese is not the default UI language");
  }
  if (!slint::select_bundled_translation("en")) {
    return fail("no bundled English translation; check ui/translations and the "
                "--bundle-translations flag in CMakeLists.txt");
  }
  app->set_language_index(1);
  if (!find_one(app, "Edit format", slint::language::AccessibleRole::Combobox)) {
    return fail("switching to English did not retranslate accessible names");
  }
  app->set_parameter_preset_index(1);
  if (!find_one(app, "Parameter preset", slint::language::AccessibleRole::Combobox) ||
      !find_one(app, "Install context menu", slint::language::AccessibleRole::Button))
    return fail("context menu preset did not translate");
  if (!find_one(app, "Changelog", slint::language::AccessibleRole::Tab)) {
    return fail("the new changelog navigation item was not translated");
  }
  if (find_one(app, "编辑格式", slint::language::AccessibleRole::Combobox)) {
    return fail("Chinese accessible name survived the switch to English");
  }
  app->set_selected_page(2);
  if (!find_one(app, "Compatibility context menu", slint::language::AccessibleRole::Checkbox))
    return fail("compatibility checkbox English translation is missing");
  // 切回中文：后面的断言仍按中文可访问名查找。
  if (!slint::select_bundled_translation("")) {
    return fail("could not switch back to the Chinese default language");
  }
  app->set_language_index(0);
  app->set_selected_page(0);
  app->set_parameter_preset_index(1);
  if (find_one(app, "查看说明", slint::language::AccessibleRole::Button) ||
      !find_one(app, "安装右键菜单", slint::language::AccessibleRole::Button))
    return fail("menu page did not start with its installation controls");
  if (!find_one(app, "编辑格式", slint::language::AccessibleRole::Combobox)) {
    return fail("switching back to Chinese did not restore the msgid text");
  }
  app->set_parameter_preset_index(0);

  app->set_selected_page(1);
  app->window().set_size(slint::LogicalSize({1220.0f, 827.0f}));
  app->set_selected_queue_index(-1);
  slint::private_api::testing::mock_elapsed_time(250);
  const auto table = slint::testing::ElementHandle::find_by_element_id(app, "QueuePage::queue-table");
  const auto header = slint::testing::ElementHandle::find_by_element_id(app, "QueuePage::queue-header");
  const auto viewport = slint::testing::ElementHandle::find_by_element_id(app, "QueuePage::queue-viewport");
  if (table.size() != 1 || header.size() != 1 || viewport.size() != 1)
    return fail("queue geometry elements are missing");
  const auto table_position = table[0].absolute_position();
  const auto table_size = table[0].size();
  const auto regions = app->get_drop_regions();
  if (regions->row_count() != 3) return fail("native drop target regions are missing");
  const std::array<std::string_view, 2> drop_labels{"输入路径", "输出目录"};
  for (std::size_t i = 0; i < drop_labels.size(); ++i) {
    const auto field = find_one(app, drop_labels[i], slint::language::AccessibleRole::TextInput);
    const auto region = regions->row_data(i);
    if (!field || !region) return fail("drop target field is missing");
    const auto position = field->absolute_position();
    const auto size = field->size();
    if (std::fabs(region->x - position.x) > 1 || std::fabs(region->y - position.y) > 1 ||
        std::fabs(region->width - size.width) > 1 || std::fabs(region->height - size.height) > 1)
      return fail("native drop region does not match its logical input field");
  }
  const auto queue_region = *regions->row_data(2);
  if (std::fabs(queue_region.x - table_position.x) > 1 ||
      std::fabs(queue_region.y - table_position.y) > 1 ||
      std::fabs(queue_region.width - table_size.width) > 1 ||
      std::fabs(queue_region.height - table_size.height) > 1)
    return fail("native drop region does not match the queue");
  if (std::fabs(header[0].absolute_position().y - table_position.y) > 0.5f)
    return fail("queue header is not anchored to the top of its table");
  const auto closed_top = viewport[0].absolute_position().y;
  app->set_selected_queue_index(0);
  slint::private_api::testing::mock_elapsed_time(250);
  const auto detail = slint::testing::ElementHandle::find_by_element_id(app, "QueuePage::queue-detail");
  if (detail.size() != 1) return fail("queue detail geometry is missing");
  const auto card = detail[0].absolute_position();
  const auto card_size = detail[0].size();
  const auto open_top = viewport[0].absolute_position().y;
  if (open_top < card.y + card_size.height || open_top > card.y + card_size.height + 8.5f ||
      std::fabs(table[0].size().height - table_size.height) > 0.5f)
    return fail("queue detail overlaps its list or resizes the queue table");
  const slint::LogicalPosition click{{card.x + card_size.width / 2, card.y + card_size.height / 2}};
  app->window().dispatch_pointer_press_event(click, slint::PointerEventButton::Left);
  if (app->get_selected_queue_index() != 0) return fail("queue detail closed before pointer release");
  app->window().dispatch_pointer_release_event(click, slint::PointerEventButton::Left);
  if (app->get_selected_queue_index() != -1) return fail("queue detail click did not close on release");
  slint::private_api::testing::mock_elapsed_time(90);
  const auto intermediate_top = viewport[0].absolute_position().y;
  if (intermediate_top <= closed_top || intermediate_top >= open_top)
    return fail("queue viewport does not animate when closing details");
  slint::private_api::testing::mock_elapsed_time(160);
  if (std::fabs(viewport[0].absolute_position().y - closed_top) > 0.5f)
    return fail("queue viewport did not recover after closing details");
  std::vector<TaskRow> scrolling_rows(80, *task_rows()->row_data(0));
  for (std::size_t i = 0; i < scrolling_rows.size(); ++i)
    scrolling_rows[i].filename = slint::SharedString{std::format("scroll-row-{:03}", i)};
  awj::ui::bind_queue_model(*app, std::make_shared<slint::VectorModel<TaskRow>>(std::move(scrolling_rows)));
  app->set_selected_queue_index(0);
  slint::private_api::testing::mock_elapsed_time(250);
  const auto scrolled_top = viewport[0].absolute_position();
  app->window().dispatch_pointer_scroll_event(
      slint::LogicalPosition({scrolled_top.x + 60, scrolled_top.y + 50}), 0, -340);
  slint::private_api::testing::mock_elapsed_time(250);
  const auto first_visible = [&] {
    std::string label;
    float first_y = 1e9f;
    const auto top = viewport[0].absolute_position().y;
    slint::testing::ElementHandle::visit_elements(app, [&](auto element) {
      const auto y = element.absolute_position().y;
      if (element.accessible_role() == slint::language::AccessibleRole::ListItem &&
          y >= top && y < first_y && element.accessible_label()) {
        first_y = y;
        label = element.accessible_label()->data();
      }
    });
    return label;
  };
  const auto anchor = first_visible();
  if (anchor.empty() || anchor.starts_with("scroll-row-000")) return fail("queue scroll check did not leave the first row");
  app->set_selected_queue_index(-1);
  slint::private_api::testing::mock_elapsed_time(250);
  if (first_visible() != anchor) return fail("closing details lost the scrolled queue anchor");
  awj::ui::bind_queue_model(*app, task_rows());
  app->set_selected_queue_index(0);
  for (const int language : {0, 1}) {
    slint::select_bundled_translation(language ? "en" : "");
    app->set_language_index(language);
    float previous_selector_width = -1.0f;
    for (const float width : {747.0f, 1171.0f, 1800.0f}) {
      if (const int result = verify_template_token_layout(app, width, language == 1);
          result != 0) return result;
      const auto selector = find_one(app, language ? "Queue preset" : "队列预设",
                                     slint::language::AccessibleRole::Combobox);
      if (!selector) return fail("queue preset selector disappeared after resize");
      if (previous_selector_width >= 0.0f &&
          std::fabs(selector->size().width - previous_selector_width) > 0.5f)
        return fail("queue selector changed its fixed width");
      previous_selector_width = selector->size().width;
    }
  }
  slint::select_bundled_translation("");
  app->set_language_index(0);
  app->window().set_size(slint::LogicalSize({820.0f, 560.0f}));
  if (!find_one(app, "输入路径",
                slint::language::AccessibleRole::TextInput) ||
      !find_one(app, "输出目录",
                slint::language::AccessibleRole::TextInput)) {
    return fail("queue path fields are missing accessible names or roles");
  }
  auto queue_list =
      find_one(app, "编码任务", slint::language::AccessibleRole::List);
  if (!queue_list || queue_list->accessible_item_count() != 2) {
    return fail("queue list is missing its role, name, or item count");
  }
  if (queue_list->size().height < 34.0f) {
    return fail(std::format(
        "queue list has less than one visible row at minimum size: {:.1f}px",
        queue_list->size().height));
  }
  app->set_queue_failed_only(true);
  if (app->get_failed_task_rows()->row_count() != 1 ||
      app->invoke_failed_row_source_index(0) != 0) {
    return fail("failure filter lost its source queue index");
  }
  {
    auto source = task_rows();
    awj::ui::bind_queue_model(*app, source);
    source->insert(0, TaskRow{});
    if (app->invoke_failed_row_source_index(0) != 1) {
      return fail("inserting a non-failed task broke filtered source indices");
    }
    source->erase(0);
    auto failed = *source->row_data(0);
    failed.state = 2;
    awj::ui::replace_queue_row(*app, source, 0, failed);
    if (app->get_failed_task_rows()->row_count() != 0 || app->get_queue_failed_count() != 0) {
      return fail("finishing a failed task did not update the projection and counters");
    }
    failed.state = 3;
    awj::ui::replace_queue_row(*app, source, 0, failed);
    if (app->get_failed_task_rows()->row_count() != 1 || app->get_queue_failed_count() != 1) {
      return fail("failed task did not return to the projection");
    }
    awj::ui::bind_queue_model(*app, task_rows());
  }
  if (!app->get_queue_failed_only() ||
      queue_list->accessible_item_count() != 1) {
    return fail("failed-only filter did not update the accessible item count");
  }
  auto retry =
      find_one(app, "重试失败", slint::language::AccessibleRole::Button);
  if (!retry || retry->accessible_role() !=
                    slint::language::AccessibleRole::Button) {
    return fail("retry button is missing its role or name");
  }
  retry->invoke_accessible_default_action();
  if (retries != 1) {
    return fail("retry accessibility action did not fire");
  }
  if (!find_one(app, "关闭详情", slint::language::AccessibleRole::Button)) {
    return fail("selected-item details did not open");
  }

  app->set_queue_failed_only(false);
  app->set_selected_queue_index(0);
  app->invoke_open_queue_menu(1, 360.0f, 220.0f);
  if (app->get_selected_queue_index() != 0) {
    return fail("opening a queue context menu changed the detail selection");
  }
  app->invoke_open_queue_menu(0, 360.0f, 220.0f);
  if (slint::cbindgen_private::slint_testing_active_popup_count(
          &app->window().window_handle()) != 1) {
    return fail("queue context menu did not open");
  }
  send_key(app, slint::platform::key_codes::End);
  send_key(app, slint::platform::key_codes::Escape);
  if (slint::cbindgen_private::slint_testing_active_popup_count(
          &app->window().window_handle()) != 0) {
    return fail("queue context menu did not handle End/Escape");
  }

  bool invalid_geometry = false;
  slint::testing::ElementHandle::visit_elements(
      app, [&invalid_geometry, scale_factor](slint::testing::ElementHandle element) {
        if (!element.accessible_role() ||
            *element.accessible_role() ==
                slint::language::AccessibleRole::None) {
          return;
        }
        const auto size = element.size();
        const auto position = element.absolute_position();
        if (size.width <= 0.0f || size.height <= 0.0f ||
            position.x + size.width < 0.0f ||
            position.y + size.height < 0.0f || position.x > 820.0f ||
            position.y > 560.0f) {
          invalid_geometry = true;
          const auto label = element.accessible_label();
          const auto id = element.id();
          std::fprintf(
              stderr,
              "clipped accessible control: scale=%.1f pos=(%.1f,%.1f) "
              "size=(%.1f,%.1f) label=%s id=%s\n",
              static_cast<double>(scale_factor), static_cast<double>(position.x),
              static_cast<double>(position.y), static_cast<double>(size.width),
              static_cast<double>(size.height),
              label ? label->data() : "<unnamed>", id ? id->data() : "<none>");
        }
      });
  if (invalid_geometry) {
    return fail("an accessible control is clipped outside the minimum window");
  }

  if (const int result = verify_parameter_matrix(app)) return result;
  if (const int result = verify_manual_update(app)) return result;
  if (const int result = verify_queue_option_layout(app)) return result;
  return verify_queue_column_resize(app);
}

// 只验证本次菜单动效，使用生产组件和模拟时钟，不触发其它页面或编码测试。
int verify_queue_menu_animation(const slint::ComponentHandle<AwjStudio>& app,
                                bool english) {
  using Role = slint::language::AccessibleRole;
  app->set_selected_page(1);
  app->set_selected_queue_index(-1);
  awj::ui::bind_queue_model(*app, task_rows());
  app->window().set_size(slint::LogicalSize({1171.0f, 800.0f}));
  slint::cbindgen_private::slint_testing_use_native_popup(&app->window().window_handle(), false);
  const auto active_popups = [&] {
    return slint::cbindgen_private::slint_testing_active_popup_count(&app->window().window_handle());
  };
  const auto verify_open = [&](bool columns, float x, float y) -> int {
    if (columns) app->invoke_open_queue_column_menu(x, y);
    else app->invoke_open_queue_menu(0, x, y);
    if (active_popups() != 1) return fail("菜单互斥打开后弹窗数量错误");
    const auto id = columns ? "AwjStudio::queue-column-menu-surface" : "AwjStudio::queue-menu-surface";
    const auto surfaces = slint::testing::ElementHandle::find_by_element_id(app, id);
    if (surfaces.size() != 1) return fail(std::format("未找到动画菜单表面 {}", id));
    const auto& surface = surfaces[0];
    const auto initial = surface.size();
    const auto initial_position = surface.absolute_position();
    if (initial.width > 1 || initial.height > 1)
      return fail("右键菜单没有从折叠状态开始展开");
    slint::private_api::testing::mock_elapsed_time(1);
    slint::private_api::testing::mock_elapsed_time(25);
    (void)surface.size();
    slint::private_api::testing::mock_elapsed_time(50);
    const auto middle = surface.size();
    const float expected_width = columns ? 220.0f : 216.0f;
    const float expected_height = columns ? 280.0f : 338.0f;
    if (!(middle.width > 1 && middle.width < expected_width - 0.1f &&
          middle.height > 1 && middle.height < expected_height - 0.1f))
      return fail(std::format("菜单缺少展开动画中间帧：columns={} size=({}, {})",
                              columns, middle.width, middle.height));
    slint::private_api::testing::mock_elapsed_time(250);
    const auto final_size = surface.size();
    const auto final_position = surface.absolute_position();
    if (std::fabs(final_size.width - expected_width) > 0.5f ||
        std::fabs(final_size.height - expected_height) > 0.5f)
      return fail("菜单动画结束后尺寸错误");
    if (initial_position.x < final_position.x - 0.5f || initial_position.y < final_position.y - 0.5f)
      return fail("菜单展开起点超出点击位置方向");
    return 0;
  };
  for (const auto point : {slint::LogicalPosition{{300, 200}}, slint::LogicalPosition{{1160, 790}}}) {
    for (int reopen = 0; reopen < 2; ++reopen) {
      if (const int result = verify_open(true, point.x, point.y)) return result;
      const auto option = find_one(app, "#", Role::Checkbox);
      if (!option || !option->accessible_checked() || !*option->accessible_checked())
        return fail("列菜单的勾选状态丢失");
      send_key(app, slint::platform::key_codes::Space);
      if (app->get_queue_index_visible()) return fail("动画后的列菜单不响应键盘切换");
      send_key(app, slint::platform::key_codes::Space);
      if (!app->get_queue_index_visible()) return fail("列菜单无法恢复勾选状态");
      if (const int result = verify_open(false, point.x, point.y)) return result;
      if (const int result = verify_open(true, point.x, point.y)) return result;
      send_key(app, slint::platform::key_codes::Escape);
      if (active_popups() != 0) return fail("列菜单不响应 Escape");
    }
  }
  app->invoke_open_queue_column_menu(300, 200);
  send_key(app, slint::platform::key_codes::Escape);
  slint::private_api::testing::mock_elapsed_time(300);
  if (active_popups() != 0) return fail("开场计时器在关闭后重新打开菜单");
  if (const int result = verify_open(true, 300, 200)) return result;
  const slint::LogicalPosition outside{{30, 200}};
  app->window().dispatch_pointer_press_event(outside, slint::PointerEventButton::Left);
  app->window().dispatch_pointer_release_event(outside, slint::PointerEventButton::Left);
  if (active_popups() != 0) return fail("列菜单不响应外部点击关闭");
  if (app->get_selected_queue_index() != -1) return fail("打开菜单修改了队列详情选择");
  std::printf("queue-menu-animation language=%s result=passed\n", english ? "en" : "zh");
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (const int result = verify_native_file_payload(); result != 0) return result;
  if (const int result = verify_shared_string_clearing(); result != 0) {
    return result;
  }
  if (argc >= 2 && std::string_view{argv[1]} == "--queue-menu-animation") {
    if (argc != 2) return fail("菜单动画回归不接受额外参数");
    slint::testing::init();
    for (const float scale : {1.0f, 1.25f, 1.5f, 2.0f}) {
      for (const int theme : {1, 2}) {
        for (const bool english : {false, true}) {
          slint::select_bundled_translation(english ? "en" : "");
          auto app = AwjStudio::create();
          app->set_current_version(AWJ_UI_VERSION);
          app->set_theme_index(theme);
          app->set_language_index(english ? 1 : 0);
          app->window().window_handle().set_const_scale_factor(scale);
          app->show();
          if (const int result = verify_queue_menu_animation(app, english)) return result;
          app->hide();
          std::printf("queue-menu-animation scale=%.2f theme=%d result=passed\n", double(scale), theme);
        }
      }
    }
    return 0;
  }
  if (argc >= 2 && std::string_view{argv[1]} == "--queue-column-regression") {
    if (argc != 2) return fail("队列列宽回归不接受额外参数");
    slint::testing::init();
    for (const float scale : {1.0f, 1.25f, 1.5f, 2.0f}) {
      auto app = AwjStudio::create();
      app->set_current_version(AWJ_UI_VERSION);
      app->window().window_handle().set_const_scale_factor(scale);
      app->show();
      if (const int result = verify_queue_column_resize(app); result != 0) return result;
      app->hide();
      std::printf("queue-columns scale=%.2f result=passed\n", double(scale));
    }
    return 0;
  }
  if (argc >= 2 && std::string_view{argv[1]} == "--queue-separator-snapshots") {
    if (argc != 4) return fail("分隔线像素回归需要输出目录和缩放参数");
    return verify_queue_separator_snapshots(std::filesystem::path{argv[2]}, std::stof(argv[3]));
  }
  if (argc >= 2 && std::string_view{argv[1]} == "--menu-rendering-snapshots") {
    if (argc != 4) return fail("菜单像素回归需要输出目录和缩放参数");
    return verify_menu_rendering_snapshots(std::filesystem::path{argv[2]}, std::stof(argv[3]));
  }
  if (argc == 4 && std::string_view{argv[1]} == "--font-rendering-snapshots") {
    return verify_font_rendering_snapshots(std::filesystem::path{argv[2]}, std::stof(argv[3]));
  }
  if ((argc == 2 || argc == 3) && std::string_view{argv[1]} == "--resize-window") {
    auto app = AwjStudio::create();
    app->set_theme_index(argc == 3 ? std::stoi(argv[2]) : 1);
    std::string text;
    for (int line = 0; line < 48; ++line) text += "窗口缩放测试 / Native resize rendering check.\n";
    std::vector<UpdateHistoryRow> history(12, *update_history_rows()->row_data(0));
    for (auto& row : history) row.changelog_zh_cn = row.changelog_en = slint::SharedString{text};
    app->set_update_history(std::make_shared<slint::VectorModel<UpdateHistoryRow>>(std::move(history)));
    app->set_selected_page(4);
    app->set_show_update_changelog(true);
    bool invalid_frame = false;
    bool reported_window = false;
    slint::Timer frames;
    frames.start(slint::TimerMode::Repeated, std::chrono::milliseconds{200}, [&] {
#ifdef _WIN32
      if (!reported_window && app->window().win32_hwnd()) {
        std::printf("resize-window,%p\n", static_cast<void*>(app->window().win32_hwnd()));
        reported_window = true;
      }
#endif
      const auto size = app->window().size();
      const auto snapshot = app->window().take_snapshot();
      const auto current_size = app->window().size();
      if (size.width != current_size.width || size.height != current_size.height) return;
      if (!snapshot || snapshot->width() != size.width || snapshot->height() != size.height) {
        invalid_frame = true;
        slint::quit_event_loop();
        return;
      }
      const bool dark = app->get_theme_index() == 2;
      const auto matches = [&](int x, int y, bool panel) {
        const auto pixel = snapshot->begin()[y * size.width + x];
        return panel ? pixel.r == (dark ? 40 : 255) && pixel.g == pixel.r && pixel.b == pixel.r
                     : pixel.r == (dark ? 36 : 238) && pixel.g == (dark ? 36 : 244) && pixel.b == (dark ? 36 : 251);
      };
      const int padding_x = static_cast<int>(169 * app->window().scale_factor());
      if (!matches(4, size.height - 10, false) ||
          !matches(size.width - 2, size.height / 2, true) ||
          !matches(padding_x, size.height - 2, true)) {
        invalid_frame = true;
        slint::quit_event_loop();
      }
      std::printf("resize-frame,%u,%u,%s\n", size.width, size.height, invalid_frame ? "fail" : "pass");
      std::fflush(stdout);
    });
    app->window().on_close_requested([] {
      slint::quit_event_loop();
      return slint::CloseRequestResponse::HideWindow;
    });
    slint::Timer timeout;
    timeout.start(slint::TimerMode::SingleShot, std::chrono::seconds{30}, [&] {
      app->hide();
      slint::quit_event_loop();
    });
    app->show();
    slint::run_event_loop();
    return invalid_frame ? fail("native resize produced an outdated size or edge") : 0;
  }
  if (argc == 2 && (std::string_view{argv[1]} == "--renderer-smoke" ||
                    std::string_view{argv[1]} == "--font-stress")) {
    const bool fonts = std::string_view{argv[1]} == "--font-stress";
    const int total_ticks = fonts ? 205 : 10;
#ifdef _WIN32
    const std::array<const char*, 3> families{"Arial", "Microsoft YaHei", "Consolas"};
#else
    const std::array<const char*, 3> families{"DejaVu Sans", "DejaVu Serif", "HarmonyOS Sans SC"};
#endif
    auto app = AwjStudio::create();
    if (const int result = verify_runtime_images(); result != 0) return result;
    awj::ui::bind_queue_model(*app, task_rows());
    app->show();
    int tick = 0;
    slint::Timer timer;
    timer.start(slint::TimerMode::Repeated, std::chrono::milliseconds{150}, [&] {
      if (tick == total_ticks) {
        app->hide();
        slint::quit_event_loop();
        return;
      }
      if (fonts) {
        if (tick < 200) app->set_ui_font_family(families[tick % families.size()]);
        if (tick == 200) {
          app->set_ui_font_family(slint::SharedString{});
          awj::ui::bind_queue_model(*app, std::make_shared<slint::VectorModel<TaskRow>>());
        }
        if (tick % 20 == 0 || tick == 204) {
          std::printf("font-stress cycle=%d\n", tick);
          std::fflush(stdout);
        }
      }
      const int language = (tick / 5) % 2;
      slint::select_bundled_translation(language ? "en" : "");
      app->set_language_index(language);
      app->set_selected_page(tick++ % 5);
    });
    slint::run_event_loop();
    timer.stop();
    return tick == total_ticks ? 0 : fail("renderer smoke exited before all pages were visited");
  }
  if (argc == 2 && std::string_view{argv[1]} == "--queue-stress") {
    slint::testing::init();
    auto app = AwjStudio::create();
    app->show();
    for (const std::size_t count : {1000u, 10000u, 100000u}) {
      std::vector<TaskRow> data;
      data.reserve(count);
      for (std::size_t i = 0; i < count; ++i) {
        auto row = *task_rows()->row_data(i % 2);
        row.order = slint::SharedString{std::to_string(i + 1)};
        data.push_back(std::move(row));
      }
      auto rows = std::make_shared<slint::VectorModel<TaskRow>>(std::move(data));
      std::weak_ptr<slint::Model<TaskRow>> lifetime = rows;
      awj::ui::bind_queue_model(*app, rows);
      app->set_queue_failed_count(static_cast<int>(count / 2));
      app->set_queue_success_count(static_cast<int>(count / 2));
      app->set_queue_failed_only(true);
      if (app->get_failed_task_rows()->row_count() != count / 2 ||
          app->invoke_failed_row_source_index(static_cast<int>(count / 2 - 1)) != static_cast<int>(count - 2)) {
        return fail("large queue filter/source mapping is incorrect");
      }
      const auto started = std::chrono::steady_clock::now();
      for (std::size_t i = 0; i < count; i += 2) {
        auto row = *rows->row_data(i);
        row.state = 2;
        awj::ui::replace_queue_row(*app, rows, i, std::move(row));
      }
      const auto elapsed = std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - started).count();
      if (app->get_failed_task_rows()->row_count() || app->get_queue_failed_count() ||
          app->get_queue_success_count() != static_cast<int>(count)) {
        return fail("large queue final events or counters were lost");
      }
      for (int cycle = 0; cycle < 30; ++cycle) {
        for (int page : {0, 1, 2, 3, 4}) {
          app->set_selected_page(page);
          slint::private_api::testing::mock_elapsed_time(250);
        }
      }
      awj::ui::bind_queue_model(*app, std::make_shared<slint::VectorModel<TaskRow>>());
      rows.reset();
      if (!lifetime.expired()) return fail("clearing the queue retained its old model");
      std::printf("queue-stress rows=%zu events=%zu elapsed_ms=%.3f pages=150 released=true\n",
                  count, count / 2, elapsed);
    }
    app->hide();
    return 0;
  }
  if ((argc >= 3 && argc <= 5) && (std::string_view{argv[1]} == "--snapshots" ||
                                 std::string_view{argv[1]} == "--scroll-snapshots" ||
                                 std::string_view{argv[1]} == "--menu-snapshots" ||
                                 std::string_view{argv[1]} == "--menu-operation-snapshots")) {
    const bool menu_operations = std::string_view{argv[1]} == "--menu-operation-snapshots";
    const bool scrolling = std::string_view{argv[1]} == "--scroll-snapshots";
    const bool menu_only = menu_operations || std::string_view{argv[1]} == "--menu-snapshots";
    const std::filesystem::path directory{argv[2]};
    if (!std::filesystem::create_directory(directory)) return fail("snapshot directory must be fresh");
    auto app = AwjStudio::create();
    const float scale = argc >= 4 ? std::stof(argv[3]) : 1.0f;
    if (scale != 1 && scale != 1.25f && scale != 1.5f && scale != 2)
      return fail("snapshot scale must be 1, 1.25, 1.5, or 2");
    app->window().window_handle().set_const_scale_factor(scale);
    app->set_current_version(AWJ_UI_VERSION);
    app->set_update_history(update_history_rows());
    if (scrolling) {
      std::vector<UpdateHistoryRow> history(64, *update_history_rows()->row_data(0));
      app->set_update_history(std::make_shared<slint::VectorModel<UpdateHistoryRow>>(std::move(history)));
    }
    app->set_only_show_changelog_after_update(true);
    app->set_show_update_changelog(true);
    app->set_update_status("已是最新版 / Already up to date — long status text for ellipsis");
    awj::ui::bind_queue_model(*app, task_rows());
    app->set_queue_failed_count(1);
    app->set_queue_success_count(1);
    app->show();
    for (const int language : {0, 1}) {
      slint::select_bundled_translation(language ? "en" : "");
      app->set_language_index(language);
      for (const int theme : {1, 2}) {
        app->set_theme_index(theme);
        for (const int page : (menu_only ? std::vector{0} : scrolling ? std::vector{0, 2, 4} : std::vector{0, 1, 2, 4})) {
          app->set_selected_page(page);
          app->set_parameter_preset_index(menu_only ? 1 : 0);
          app->set_selected_queue_index(page == 1 ? 0 : -1);
          const slint::PhysicalSize size{{static_cast<std::uint32_t>(std::lround((argc == 5 ? std::stof(argv[4]) : menu_only ? 900 : 1171) * scale)),
                                         static_cast<std::uint32_t>(std::lround((page == 1 ? 827 : scrolling ? 560 : 2000) * scale))}};
          app->window().set_size(size);
          for (int format = 0; format < (menu_operations ? 3 : scrolling || page == 1 || page == 2 || page == 4 || menu_only ? 1 : 5); ++format) {
            app->set_format_index(menu_operations ? 0 : format);
            app->set_menu_format_index(menu_operations ? 0 : format);
            // Snapshot 0 is idle, 1 is ordinary registration, 2 requires UAC.
            app->set_menu_operation_active(menu_operations && format != 0);
            app->set_menu_elevation_required(menu_operations && format == 2);
            for (int stage = 0; stage < (scrolling ? 3 : 1); ++stage) {
            if (stage > 0) app->window().dispatch_pointer_scroll_event(
                slint::LogicalPosition({static_cast<float>(size.width / scale - 30), 300}),
                0, stage == 1 ? -220 : -100000);
            (void)app->window().take_snapshot();
            if (scrolling) {
              std::this_thread::sleep_for(std::chrono::milliseconds{250});
              slint::platform::update_timers_and_animations();
            } else {
              slint::private_api::testing::mock_elapsed_time(250);
            }
            const auto snapshot = app->window().take_snapshot();
            if (!snapshot) return fail("Slint renderer could not produce a snapshot");
            if (snapshot->width() != size.width || snapshot->height() != size.height ||
                std::fabs(app->window().scale_factor() - scale) > 0.001f)
              return fail("snapshot did not use the requested pixel size and scale");
            const auto name = std::format("lang{}-theme{}-page{}-format{}", language, theme, page, format) +
                (scrolling ? std::format("-scroll{}.ppm", stage) : ".ppm");
            if (!write_snapshot_ppm(directory / name, *snapshot))
              return fail("could not write the renderer snapshot");
            }
          }
        }
      }
    }
    app->hide();
    return 0;
  }
  slint::testing::init();
  if (const int result = verify_runtime_images(); result != 0) return result;
  // 指针和弹出窗口回归使用 testing-only hooks，不能放进真实 renderer 分支。
  if (const int result = verify_font_hover_stability(); result != 0) return result;
  {
    auto focus = awj_focus_test::FocusReentrancy::create();
    focus->show();
    for (int i = 0; i < 100; ++i) {
      focus->window().dispatch_window_active_changed_event(true);
      focus->invoke_prepare();
      focus->window().dispatch_window_active_changed_event(false);
      if (focus->get_transitions() != i + 1) return fail("focus reentrancy regression did not exercise its callback");
    }
    focus->hide();
  }
  // Slint 的 const scale setter 只接受首次设置，各比例必须使用新窗口。
  for (const float scale : {1.0f, 1.25f, 1.5f, 2.0f}) {
    auto app = AwjStudio::create();
    if (app->get_speed_text() != "5" || app->get_menu_speed_text() != "5")
      return fail("initial AVIF speed is not 5");
    app->show();
    const int result = run_scale(app, scale);
    app->hide();
    if (result != 0) return result;
  }
  return 0;
}
