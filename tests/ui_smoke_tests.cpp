#include <slint.h>
#include <private/slint_tests_helpers.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "awj_studio_ui_smoke.h"
#include "focus_reentrancy.h"
#include "../src/ui/queue_model.h"
#include "../src/ui/deferred_model.h"

namespace {

int fail(std::string_view message) {
  std::fwrite(message.data(), 1, message.size(), stderr);
  std::fputc('\n', stderr);
  return 1;
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
                                 float window_width, bool expect_two_rows) {
  app->window().set_size(slint::LogicalSize({window_width, 560.0f}));
  const auto collision = find_one(app, "重复处理", slint::language::AccessibleRole::Combobox);
  if (!collision || collision->absolute_position().x + collision->size().width > window_width - 10)
    return fail("collision dropdown overflows the queue header");
  const std::array<std::string_view, 6> labels{"参数", "日期", "时间",
                                                "随机", "哈希", "SHA"};
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
  if (expected_width < 67.5f) {
    return fail(std::format("template token button width is too small: {:.1f}px",
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
  if (expect_two_rows) {
    if (std::fabs(y(0) - y(1)) > 0.5f || std::fabs(y(1) - y(2)) > 0.5f ||
        std::fabs(y(3) - y(4)) > 0.5f || std::fabs(y(4) - y(5)) > 0.5f ||
        y(3) <= y(0) + 1.0f) {
      return fail("template token buttons did not form two aligned rows");
    }
    if (std::fabs(x(0) - x(3)) > 0.5f || std::fabs(x(1) - x(4)) > 0.5f ||
        std::fabs(x(2) - x(5)) > 0.5f) {
      return fail("template token two-row columns are not aligned");
    }
  } else {
    for (std::size_t index = 1; index < buttons.size(); ++index) {
      if (std::fabs(y(index) - y(0)) > 0.5f || x(index) <= x(index - 1)) {
        return fail("template token buttons did not form one aligned row");
      }
    }
  }
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

int verify_parameter_matrix(const slint::ComponentHandle<AwjStudio>& app) {
  using Role = slint::language::AccessibleRole;
  app->window().set_size(slint::LogicalSize({1220.0f, 2000.0f}));
  for (const int language : {0, 1}) {
    if (!slint::select_bundled_translation(language ? "en" : "")) return fail("missing matrix translation");
    app->set_language_index(language);
    for (const int theme : {1, 2}) {
      app->set_theme_index(theme);
      for (const int page : {0, 3}) {
        app->set_selected_page(page);
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
              check("视觉质量", "Visual quality", Role::TextInput, page == 0 && format != 4) ||
              check("速度", "Speed", Role::TextInput, format < 3) ||
              check("线程", "Threads", Role::TextInput, page == 0) ||
              check("内存限制", "Memory limit", Role::TextInput, page == 0) ||
              check("位深", "Bit depth", Role::TextInput, format == 0) ||
              check("色度采样", "Chroma subsampling", Role::Combobox, format == 0 || format == 3) ||
              check("JPGLI 渐进级别", "JPGLI progressive level", Role::Combobox, format == 3) ||
              check("启用 XYB", "Enable XYB", Role::Checkbox, format == 3)) return 1;
          if (format == 3) {
            for (const int progressive : {0, 1, 2}) {
              if (page == 0) app->set_jpegli_progressive_index(progressive);
              else app->set_menu_jpegli_progressive_index(progressive);
              slint::private_api::testing::mock_elapsed_time(1);
              if (check("优化哈夫曼表", "Optimize Huffman tables", Role::Checkbox, progressive == 0)) return 1;
              if (progressive > 0 && !(page == 0 ? app->get_jpegli_optimize_huffman() : app->get_menu_jpegli_optimize_huffman()))
                return fail("progressive JPGLI did not force Huffman optimization");
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
    for (const float width : {820.0f, 1220.0f, 1440.0f}) {
      app->window().set_size(slint::LogicalSize({width, 827.0f}));
      const auto& labels = language ? en : zh;
      float option_width = 0;
      float first_y = 0;
      const bool single_row = width >= (language ? 1400 : 1100);
      for (std::size_t i = 0; i < labels.size(); ++i) {
        const auto option = find_one(app, labels[i], Role::Checkbox);
        if (!option) return fail("queue option is missing");
        if (i == 0) { option_width = option->size().width; first_y = option->absolute_position().y; }
        if (std::fabs(option->size().width - option_width) > 0.5f ||
            option->absolute_position().x + option->size().width > width - 10 ||
            (language && option->size().width < 180))
          return fail("queue options are unequal, clipped, or too narrow for English");
        if ((i < 3 || single_row) != (std::fabs(option->absolute_position().y - first_y) < 0.5f))
          return fail("queue options did not wrap into the expected equal columns");
      }
      const auto format = find_one(app, language ? "Queue output format" : "队列输出格式", Role::Combobox);
      const auto preset = find_one(app, language ? "Queue preset" : "队列预设", Role::Combobox);
      if (!format || !preset || std::fabs(format->size().width - preset->size().width) > 0.5f ||
          preset->absolute_position().x + preset->size().width > width - 10)
        return fail("queue format/preset selectors are unequal or clipped");
    }
  }
  slint::select_bundled_translation("");
  app->set_language_index(0);
  return 0;
}

// 验证队列固定列宽可拖动，且表头与数据行同步。
// 拖动手柄改变的是 root.queue-*-width；表头与数据行都读同一属性，
// 所以只要改属性后两侧的列位置一致偏移，就说明对齐关系成立。
int verify_queue_column_resize(const slint::ComponentHandle<AwjStudio>& app) {
  using Role = slint::language::AccessibleRole;
  app->set_selected_page(1);
  app->set_queue_failed_only(false);
  app->set_task_rows(task_rows());
  app->set_selected_queue_index(-1);
  app->window().set_size(slint::LogicalSize({1220.0f, 827.0f}));
  slint::select_bundled_translation("");

  const auto header_filename = find_one(app, "文件名", Role::Text);
  const auto header_size = find_one(app, "大小", Role::Text);
  const auto header_status = find_one(app, "状态", Role::Text);
  const auto header_log = find_one(app, "输出或日志", Role::Text);
  const auto header_dir = find_one(app, "目录", Role::Text);
  if (!header_filename || !header_size || !header_status) {
    return fail("queue header columns are missing");
  }
  const auto row_filename = find_one(app, "completed.png", Role::Text);
  const auto row_status = find_one(app, "完成", Role::Text);
  const auto row_size = find_one(app, "1.0 MiB", Role::Text);
  if (!row_filename || !row_status || !row_size) {
    return fail("queue data row columns are missing");
  }

  const auto header_size_x = header_size->absolute_position().x;
  const auto header_status_x = header_status->absolute_position().x;
  const auto row_size_x = row_size->absolute_position().x;
  const auto row_status_x = row_status->absolute_position().x;

  // 核心修复：表头与数据行的固定列必须逐列对齐。两者处在不同容器里
  // （表头在 queue-table 下，数据行在 ListView 视口里），错位正是本次要修的。
  if (std::fabs(header_size_x - row_size_x) > 2.0f) {
    return fail(std::format(
        "header size column misaligned with data row: header={} row={}",
        header_size_x, row_size_x));
  }
  if (std::fabs(header_status_x - row_status_x) > 2.0f) {
    return fail(std::format(
        "header status column misaligned with data row: header={} row={}",
        header_status_x, row_status_x));
  }

  // 拖动文件名列：该列自身应变宽（拖拽生效），且表头与数据行仍逐列对齐。
  const auto before_filename_width = header_filename->size().width;
  app->set_queue_filename_width(before_filename_width + 60.0f);
  if (std::fabs(header_filename->size().width - (before_filename_width + 60.0f)) > 1.0f) {
    return fail(std::format("filename column did not widen: {} -> {}",
                            before_filename_width, header_filename->size().width));
  }
  if (std::fabs(row_filename->size().width - header_filename->size().width) > 1.0f) {
    return fail("filename column width diverged between header and data row");
  }
  if (std::fabs(header_size->absolute_position().x - row_size->absolute_position().x) > 2.0f ||
      std::fabs(header_status->absolute_position().x - row_status->absolute_position().x) > 2.0f) {
    return fail("fixed columns lost alignment after resize");
  }
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
      std::pair{"菜单参数", 3}, std::pair{"更新日志", 4},
      std::pair{"设置", 2}};
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
  if (app->get_selected_page() != 3) {
    return fail("second Up/Space navigation did not reach menu settings");
  }
  send_key(app, slint::platform::key_codes::Home);
  send_key(app, slint::platform::key_codes::Return);
  if (app->get_selected_page() != 1) {
    return fail("Home navigation failed");
  }

  auto settings =
      find_one(app, "设置", slint::language::AccessibleRole::Tab);
  settings->invoke_accessible_default_action();
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
  if (!find_one(app, "Changelog", slint::language::AccessibleRole::Tab)) {
    return fail("the new changelog navigation item was not translated");
  }
  if (find_one(app, "编辑格式", slint::language::AccessibleRole::Combobox)) {
    return fail("Chinese accessible name survived the switch to English");
  }
  // 切回中文：后面的断言仍按中文可访问名查找。
  if (!slint::select_bundled_translation("")) {
    return fail("could not switch back to the Chinese default language");
  }
  app->set_language_index(0);
  if (!find_one(app, "编辑格式", slint::language::AccessibleRole::Combobox)) {
    return fail("switching back to Chinese did not restore the msgid text");
  }

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
  if (const int result = verify_template_token_layout(app, 820.0f, true);
      result != 0) {
    return result;
  }
  if (const int result = verify_template_token_layout(app, 1220.0f, false);
      result != 0) {
    return result;
  }
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
  if (const int result = verify_queue_option_layout(app)) return result;
  return verify_queue_column_resize(app);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::string_view{argv[1]} == "--queue-stress") {
    slint::testing::init();
    auto app = AwjStudio::create();
    app->show();
    for (const std::size_t count : {1000u, 10000u}) {
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
  if ((argc == 3 || argc == 4) && std::string_view{argv[1]} == "--snapshots") {
    const std::filesystem::path directory{argv[2]};
    if (!std::filesystem::create_directory(directory)) return fail("snapshot directory must be fresh");
    auto app = AwjStudio::create();
    const float scale = argc == 4 ? std::stof(argv[3]) : 1.0f;
    if (scale != 1 && scale != 1.25f && scale != 1.5f && scale != 2)
      return fail("snapshot scale must be 1, 1.25, 1.5, or 2");
    app->window().window_handle().set_const_scale_factor(scale);
    app->set_current_version("1.0.12");
    awj::ui::bind_queue_model(*app, task_rows());
    app->set_queue_failed_count(1);
    app->set_queue_success_count(1);
    app->show();
    for (const int language : {0, 1}) {
      slint::select_bundled_translation(language ? "en" : "");
      app->set_language_index(language);
      for (const int theme : {1, 2}) {
        app->set_theme_index(theme);
        for (const int page : {0, 1, 3}) {
          app->set_selected_page(page);
          app->set_selected_queue_index(page == 1 ? 0 : -1);
          const slint::PhysicalSize size{{static_cast<std::uint32_t>(std::lround(1220 * scale)),
                                         static_cast<std::uint32_t>(std::lround((page == 1 ? 827 : 2000) * scale))}};
          app->window().set_size(size);
          for (int format = 0; format < (page == 1 ? 1 : 5); ++format) {
            app->set_format_index(format);
            app->set_menu_format_index(format);
            (void)app->window().take_snapshot();
            slint::private_api::testing::mock_elapsed_time(250);
            const auto snapshot = app->window().take_snapshot();
            if (!snapshot) return fail("Slint renderer could not produce a snapshot");
            if (snapshot->width() != size.width || snapshot->height() != size.height ||
                std::fabs(app->window().scale_factor() - scale) > 0.001f)
              return fail("snapshot did not use the requested pixel size and scale");
            std::ofstream file{directory / std::format("lang{}-theme{}-page{}-format{}.ppm", language, theme, page, format), std::ios::binary};
            file << "P6\n" << snapshot->width() << ' ' << snapshot->height() << "\n255\n";
            for (const auto& pixel : *snapshot) {
              file.put(static_cast<char>(pixel.r));
              file.put(static_cast<char>(pixel.g));
              file.put(static_cast<char>(pixel.b));
            }
            if (!file) return fail("could not write the renderer snapshot");
          }
        }
      }
    }
    app->hide();
    return 0;
  }
  slint::testing::init();
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
  auto app = AwjStudio::create();
  app->show();
  for (const float scale : {1.0f, 1.25f, 1.5f, 2.0f}) {
    if (const int result = run_scale(app, scale); result != 0) {
      return result;
    }
  }
  app->hide();
  return 0;
}
