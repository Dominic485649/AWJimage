#include "import_service.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <format>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#endif

namespace awj::ui_import {
namespace {

bool is_reparse_or_symlink(const std::filesystem::path& path) {
  std::error_code ec;
  if (std::filesystem::is_symlink(std::filesystem::symlink_status(path, ec)) && !ec) return true;
#ifdef _WIN32
  const DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
  return false;
#endif
}

void add_error(Result& result, std::string text) {
  if (result.errors.size() < 16) result.errors.push_back(std::move(text));
}

bool stop_requested(const Options& options) {
  return options.stop_requested && options.stop_requested();
}

void append_file(Result& result, const std::filesystem::path& path,
                 const std::filesystem::path& source_root, const Options& options) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec) || ec) {
    ++result.inaccessible_count;
    return;
  }
  if (options.is_supported && !options.is_supported(path)) {
    ++result.unsupported_count;
    return;
  }
  const auto bytes = std::filesystem::file_size(path, ec);
  if (ec) {
    ++result.inaccessible_count;
    add_error(result, std::format("读取文件大小失败: {}", path.string()));
    return;
  }
  if (options.maximum_file_bytes != 0 && bytes > options.maximum_file_bytes) {
    ++result.unsupported_count;
    add_error(result, std::format("输入文件超过当前输入上限: {}", path.string()));
    return;
  }
  // 先完成有效性检查，失败的末次出现不能替换之前的有效输入。
  result.files.push_back(File{.path = path, .source_root = source_root, .bytes = bytes});
}

void scan_directory(Result& result, const std::filesystem::path& root, const Options& options) {
  const auto first = result.files.size();
  std::vector<std::filesystem::path> pending{root};
  while (!pending.empty()) {
    if (stop_requested(options)) { result.cancelled = true; return; }
    auto current = std::move(pending.back());
    pending.pop_back();
    std::error_code ec;
    std::filesystem::directory_iterator it{
        current, std::filesystem::directory_options::skip_permission_denied, ec};
    if (ec) {
      ++result.inaccessible_count;
      add_error(result, std::format("扫描文件夹失败: {}", current.string()));
      continue;
    }
    for (std::filesystem::directory_iterator end; it != end; it.increment(ec)) {
      if (stop_requested(options)) { result.cancelled = true; return; }
      if (ec) { ++result.inaccessible_count; ec.clear(); continue; }
      const auto entry_path = it->path();
      const auto status = it->symlink_status(ec);
      if (ec) { ++result.inaccessible_count; ec.clear(); continue; }
      if (std::filesystem::is_directory(status)) {
        // 不跟随目录符号链接/junction/reparse point。
        if (!is_reparse_or_symlink(entry_path)) pending.push_back(entry_path);
      } else if (std::filesystem::is_regular_file(status)) {
        append_file(result, entry_path, root, options);
      }
    }
  }
  // 每个输入根内部确定性排序，不改变多个输入根之间的先后顺序。
  std::sort(result.files.begin() + static_cast<std::ptrdiff_t>(first), result.files.end(),
            [](const File& a, const File& b) { return a.path.native() < b.path.native(); });
}

}  // namespace

ui_queue::PathKey normalized_path_key(const std::filesystem::path& path) {
  return ui_queue::path_key(path);
}

Result scan(const std::vector<Root>& roots, Origin /*origin*/, const Options& options) {
  Result result;
  try {
    for (const auto& root : roots) {
      if (stop_requested(options)) { result.cancelled = true; break; }
      std::error_code ec;
      const bool directory = root.force_directory || std::filesystem::is_directory(root.path, ec);
      if (ec) {
        ++result.inaccessible_count;
        add_error(result, std::format("无法访问输入路径: {}", root.path.string()));
        continue;
      }
      if (directory) {
        if (!std::filesystem::is_directory(root.path, ec) || ec) {
          ++result.inaccessible_count;
          add_error(result, std::format("不是可扫描的文件夹: {}", root.path.string()));
          continue;
        }
        if (is_reparse_or_symlink(root.path)) {
          ++result.inaccessible_count;
          add_error(result, std::format("拒绝递归扫描符号链接或 reparse 文件夹: {}", root.path.string()));
          continue;
        }
        scan_directory(result, root.path, options);
        if (result.cancelled) break;
      } else {
        append_file(result, root.path, {}, options);
      }
    }
    if (stop_requested(options)) result.cancelled = true;
    if (result.cancelled) { result.files.clear(); return result; }
    auto files = std::move(result.files);
    const auto plan = ui_queue::plan_merge(0, files.size(),
        [](std::size_t) { return ui_queue::PathKey{}; },
        [&](std::size_t i) { return normalized_path_key(files[i].path); });
    result.files.reserve(plan.entries.size());
    for (const auto& entry : plan.entries) result.files.push_back(std::move(files[entry.incoming]));
    result.duplicate_count = plan.folded;
  } catch (const std::bad_alloc&) {
    result.failed = true;
    result.files.clear();
    add_error(result, "导入扫描时内存不足。");
  } catch (const std::length_error&) {
    result.failed = true;
    result.files.clear();
    add_error(result, "导入扫描数量超过运行时限制。");
  } catch (const std::filesystem::filesystem_error& error) {
    result.failed = true;
    result.files.clear();
    add_error(result, std::format("导入扫描时文件系统访问失败: {}", error.what()));
  }
  return result;
}

std::string summary_text(const Result& result, std::size_t added, std::size_t requeued) {
  if (result.cancelled) return "导入扫描已取消。";
  if (added + requeued == 0) {
    if (!result.errors.empty()) return result.errors.front();
    return std::format("没有可导入的图片{}{}。",
        result.unsupported_count ? std::format("，{} 个格式/大小不支持", result.unsupported_count) : "",
        result.inaccessible_count ? std::format("，{} 个不可访问", result.inaccessible_count) : "");
  }
  return std::format("新增 {} 张，重新入队 {} 张{}{}{}。", added, requeued,
      result.duplicate_count ? std::format("，按末次导入折叠 {} 个重复项", result.duplicate_count) : "",
      result.unsupported_count ? std::format("，{} 个格式/大小不支持", result.unsupported_count) : "",
      result.inaccessible_count ? std::format("，{} 个不可访问", result.inaccessible_count) : "");
}

struct Dispatcher::Impl {
  Impl(OptionsProvider options_provider, Completion completion)
      : options_provider(std::move(options_provider)), completion(std::move(completion)),
        worker([this](std::stop_token token) { run(token); }) {}

  void run(std::stop_token token) {
    while (!token.stop_requested()) {
      Request request;
      std::uint64_t generation{};
      {
        std::unique_lock lock{mutex};
        cv.wait(lock, token, [&] { return !jobs.empty(); });
        if (token.stop_requested()) break;
        if (jobs.empty()) continue;
        request = std::move(jobs.front());
        jobs.pop_front();
        generation = cancel_generation.load();
      }
      auto scan_options = options_provider ? options_provider() : Options{};
      scan_options.stop_requested = [this, token, generation, previous = std::move(scan_options.stop_requested)] {
        return token.stop_requested() || cancel_generation.load() != generation || (previous && previous());
      };
      auto result = scan(request.roots, request.origin, scan_options);
      if (token.stop_requested()) break;
      if (completion) completion(std::move(request), std::move(result));
    }
  }

  OptionsProvider options_provider;
  Completion completion;
  std::mutex mutex;
  std::condition_variable_any cv;
  std::deque<Request> jobs;
  std::atomic_uint64_t cancel_generation{};
  std::jthread worker;
};

Dispatcher::Dispatcher(OptionsProvider options_provider, Completion completion)
    : impl_(std::make_unique<Impl>(std::move(options_provider), std::move(completion))) {}
Dispatcher::~Dispatcher() { request_stop(); join(); }
Dispatcher::Dispatcher(Dispatcher&&) noexcept = default;
Dispatcher& Dispatcher::operator=(Dispatcher&&) noexcept = default;

bool Dispatcher::enqueue(Request request) {
  if (!impl_ || impl_->worker.get_stop_token().stop_requested()) return false;
  {
    std::scoped_lock lock{impl_->mutex};
    if (impl_->worker.get_stop_token().stop_requested() || impl_->jobs.size() >= 64) return false;
    impl_->jobs.push_back(std::move(request));
  }
  impl_->cv.notify_one();
  return true;
}
void Dispatcher::discard_pending() {
  if (!impl_) return;
  std::scoped_lock lock{impl_->mutex};
  ++impl_->cancel_generation;
  impl_->jobs.clear();
}
void Dispatcher::request_stop() {
  if (!impl_) return;
  impl_->worker.request_stop();
  impl_->cv.notify_all();
}
void Dispatcher::join() {
  if (impl_ && impl_->worker.joinable()) impl_->worker.join();
}
bool Dispatcher::joinable() const noexcept { return impl_ && impl_->worker.joinable(); }

}  // namespace awj::ui_import
