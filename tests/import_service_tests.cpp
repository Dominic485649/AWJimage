#include "../src/ui/import_service.h"

#include <atomic>
#include <chrono>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

namespace {

int fail(std::string_view message) {
  std::cerr << message << '\n';
  return 1;
}

bool supported(const fs::path& path) {
  auto ext = path.extension().wstring();
  for (auto& ch : ext) ch = static_cast<wchar_t>(std::towlower(ch));
  return ext == L".png" || ext == L".jpg";
}

void write_file(const fs::path& path, std::string_view bytes) {
  std::ofstream output{path, std::ios::binary};
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

}  // namespace

int main() {
  const auto root = fs::temp_directory_path() / "awj-import-service-tests";
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root / "nested", ec);
  if (ec) return fail("failed to create test tree");

  write_file(root / "a.png", "abc");
  write_file(root / "nested" / "b.JPG", "abcd");
  write_file(root / "nested" / "ignored.txt", "x");
  write_file(root / "too-large.png", "123456789");
  fs::create_directories(root / "empty", ec);
  const auto direct_root = fs::temp_directory_path() / "awj-import-service-direct.jpg";
  write_file(direct_root, "five!");

  const awj::ui_import::Options options{.is_supported = supported,
                                        .stop_requested = {},
                                        .maximum_file_bytes = 8};
  const auto result = awj::ui_import::scan(
      {{.path = root, .force_directory = true},
       {.path = root / "a.png"},
       {.path = direct_root}},
      awj::ui_import::Origin::drag_drop, options);

  if (result.files.size() != 3) {
    fs::remove_all(root, ec);
    fs::remove(direct_root, ec);
    return fail("expected folder files plus one direct mixed-root file");
  }
  if (result.duplicate_count != 1) {
    fs::remove_all(root, ec);
    fs::remove(direct_root, ec);
    return fail("explicit duplicate root was not deduplicated");
  }
  if (result.unsupported_count != 2) {
    fs::remove_all(root, ec);
    fs::remove(direct_root, ec);
    return fail("unsupported/oversized counters are incorrect");
  }
  std::size_t folder_files = 0;
  std::size_t direct_files = 0;
  for (const auto& file : result.files) {
    if (file.path == direct_root) {
      ++direct_files;
      if (!file.source_root.empty()) {
        fs::remove_all(root, ec);
        fs::remove(direct_root, ec);
        return fail("direct file must not be assigned a folder source_root");
      }
    } else if (!file.source_root.empty()) {
      ++folder_files;
    }
  }
  // 显式文件在目录之后导入，应以最后一次元数据为准，清除旧 source_root。
  if (folder_files != 1 || direct_files != 1) {
    fs::remove_all(root, ec);
    fs::remove(direct_root, ec);
    return fail("mixed file/folder classification is incorrect");
  }

  write_file(root / "z.png", "last");
  const auto ordered = awj::ui_import::scan(
      {{.path = root / "z.png"}, {.path = root / "a.png"},
       {.path = root / "." / "z.png"}, {.path = direct_root}},
      awj::ui_import::Origin::drag_drop, options);
  if (ordered.files.size() != 3 || ordered.duplicate_count != 1 ||
      ordered.files[0].path != root / "a.png" ||
      ordered.files[1].path != root / "." / "z.png" ||
      ordered.files[2].path != direct_root)
    return fail("末次有效导入的位置被首次去重或全局路径排序覆盖");
  int occurrences = 0;
  auto last_invalid_options = options;
  last_invalid_options.is_supported = [&](const fs::path&) { return ++occurrences != 2; };
  const auto last_invalid = awj::ui_import::scan(
      {{.path = root / "a.png"}, {.path = root / "a.png"}},
      awj::ui_import::Origin::drag_drop, last_invalid_options);
  if (last_invalid.files.size() != 1 || last_invalid.unsupported_count != 1 ||
      last_invalid.duplicate_count != 0)
    return fail("无效的末次输入覆盖了先前有效输入");
#ifndef _WIN32
  const auto native_path = root / fs::path{u8"中文 名称.png"};
  write_file(native_path, "utf8");
  const auto native = awj::ui_import::scan({{.path = native_path}},
      awj::ui_import::Origin::file_dialog, options);
  if (native.failed || native.files.size() != 1 ||
      awj::ui_import::normalized_path_key(native_path).empty())
    return fail("Linux 原生 UTF-8 路径不应依赖宽字符 locale 转换");
  if (awj::ui_import::normalized_path_key(root / "A.png") ==
      awj::ui_import::normalized_path_key(root / "a.png"))
    return fail("Linux 路径大小写被错误折叠");
#endif

  const auto empty = awj::ui_import::scan(
      {{.path = root / "empty", .force_directory = true}},
      awj::ui_import::Origin::folder_dialog, options);
  if (!empty.files.empty() || empty.inaccessible_count != 0) {
    fs::remove_all(root, ec);
    fs::remove(direct_root, ec);
    return fail("empty folder should be a valid import with zero files");
  }

  const auto missing = awj::ui_import::scan(
      {{.path = root / "missing.png"}}, awj::ui_import::Origin::file_dialog,
      options);
  if (!missing.files.empty() || missing.inaccessible_count != 1) {
    fs::remove_all(root, ec);
    fs::remove(direct_root, ec);
    return fail("missing root was not classified as inaccessible");
  }

  const auto cancelled = awj::ui_import::scan(
      {{.path = root, .force_directory = true}}, awj::ui_import::Origin::drag_drop,
      awj::ui_import::Options{.is_supported = supported,
                              .stop_requested = [] { return true; },
                              .maximum_file_bytes = 8});
  if (!cancelled.cancelled || !cancelled.files.empty()) {
    fs::remove_all(root, ec);
    return fail("cancelled scan did not stop before importing files");
  }

  std::atomic<int> options_provider_calls{};
  std::atomic<int> completion_count{};
  std::promise<void> completion;
  auto completed = completion.get_future();
  awj::ui_import::Dispatcher dispatcher(
      [&] {
        ++options_provider_calls;
        return options;
      },
      [&](awj::ui_import::Request request, awj::ui_import::Result dispatched) {
        if (request.origin == awj::ui_import::Origin::file_dialog &&
            dispatched.files.size() == 1) {
          if (++completion_count == 2) completion.set_value();
        }
      });
  for (const auto& path : {root / "a.png", direct_root}) {
    if (!dispatcher.enqueue(awj::ui_import::Request{
            .roots = {{.path = path}},
            .origin = awj::ui_import::Origin::file_dialog,
            .input_hint = path,
            .update_input_path = true})) {
      fs::remove_all(root, ec);
      fs::remove(direct_root, ec);
      return fail("dispatcher rejected a valid consecutive import request");
    }
  }
  if (completed.wait_for(std::chrono::seconds{5}) != std::future_status::ready) {
    fs::remove_all(root, ec);
    fs::remove(direct_root, ec);
    return fail("dispatcher did not complete consecutive queued imports");
  }
  completed.get();
  if (completion_count.load() != 2 || options_provider_calls.load() != 2) {
    fs::remove_all(root, ec);
    fs::remove(direct_root, ec);
    return fail("dispatcher did not scan each queued import with per-job options");
  }
  dispatcher.request_stop();
  dispatcher.join();

  std::promise<void> entered, release_scan, after_clear;
  auto entered_future = entered.get_future();
  auto release_future = release_scan.get_future().share();
  auto after_clear_future = after_clear.get_future();
  std::atomic<int> providers{}, callbacks{}, clear_mask{};
  awj::ui_import::Dispatcher clear_dispatcher(
      [&] {
        if (++providers == 1) { entered.set_value(); release_future.wait(); }
        return options;
      },
      [&](awj::ui_import::Request request, awj::ui_import::Result dispatched) {
        if (request.epoch == 10 && dispatched.cancelled && dispatched.files.empty()) clear_mask.fetch_or(1);
        if (request.epoch == 11 && !dispatched.cancelled && dispatched.files.size() == 1) clear_mask.fetch_or(2);
        if (++callbacks == 2) after_clear.set_value();
      });
  clear_dispatcher.enqueue({.roots = {{.path = direct_root}}, .epoch = 10});
  if (entered_future.wait_for(std::chrono::seconds{5}) != std::future_status::ready) {
    release_scan.set_value();
    return fail("取消边界测试的扫描线程未启动");
  }
  clear_dispatcher.enqueue({.roots = {{.path = root / "a.png"}}, .epoch = 10});
  clear_dispatcher.discard_pending();
  const bool new_accepted = clear_dispatcher.enqueue({.roots = {{.path = root / "a.png"}}, .epoch = 11});
  release_scan.set_value();
  if (!new_accepted || after_clear_future.wait_for(std::chrono::seconds{5}) != std::future_status::ready)
    return fail("清空后新请求被错误停止或丢失");
  clear_dispatcher.request_stop();
  clear_dispatcher.join();
  if (providers != 2 || callbacks != 2 || clear_mask != 3)
    return fail("清空未取消当前/旧排队扫描，或污染了新 epoch");

  const auto upper_key = awj::ui_import::normalized_path_key(root / "A.PNG");
  const auto lower_key = awj::ui_import::normalized_path_key(root / "a.png");
#ifdef _WIN32
  if (upper_key != lower_key) {
    fs::remove_all(root, ec);
    return fail("Windows dedupe key must be case-insensitive");
  }
#endif

  // Directory symlinks/junction-like reparse entries must never be followed by the scanner.
  const auto outside = fs::temp_directory_path() / "awj-import-service-outside";
  fs::remove_all(outside, ec);
  fs::create_directories(outside, ec);
  write_file(outside / "outside.png", "x");
  ec.clear();
  fs::create_directory_symlink(outside, root / "nested" / "linked-outside", ec);
  if (!ec) {
    const auto no_follow = awj::ui_import::scan(
        {{.path = root, .force_directory = true}}, awj::ui_import::Origin::folder_dialog,
        options);
    for (const auto& file : no_follow.files) {
      if (file.path.filename() == L"outside.png") {
        fs::remove_all(root, ec);
        fs::remove_all(outside, ec);
        fs::remove(direct_root, ec);
        return fail("scanner followed a directory symlink/reparse point");
      }
    }
  }

  fs::remove_all(root, ec);
  fs::remove_all(outside, ec);
  fs::remove(direct_root, ec);
  std::cout << "import service tests passed\n";
  return 0;
}
