#include "import_service.h"

#include <algorithm>
#include <condition_variable>
#include <cwctype>
#include <deque>
#include <format>
#include <mutex>
#include <system_error>
#include <thread>
#include <unordered_set>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#endif

namespace awj::ui_import {
namespace {

bool is_reparse_or_symlink(const std::filesystem::path& path) {
  std::error_code ec;
  if (std::filesystem::is_symlink(std::filesystem::symlink_status(path, ec)) && !ec) {
    return true;
  }
#ifdef _WIN32
  const DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES &&
         (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
  return false;
#endif
}

void add_error(Result& result, std::string text) {
  // 保留有限的代表性错误，避免超大目录把 UI 状态和内存都塞满。
  constexpr std::size_t kMaximumStoredErrors = 16;
  if (result.errors.size() < kMaximumStoredErrors) {
    result.errors.push_back(std::move(text));
  }
}

bool stop_requested(const Options& options) {
  return options.stop_requested && options.stop_requested();
}

void append_file(Result& result, std::unordered_set<std::wstring>& seen,
                 const std::filesystem::path& path,
                 const std::filesystem::path& source_root,
                 const Options& options) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec) || ec) {
    ++result.inaccessible_count;
    return;
  }
  if (options.is_supported && !options.is_supported(path)) {
    ++result.unsupported_count;
    return;
  }
  const auto key = normalized_path_key(path);
  if (!seen.insert(key).second) {
    ++result.duplicate_count;
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
  result.files.push_back(File{.path = path, .source_root = source_root, .bytes = bytes});
}

void scan_directory(Result& result, std::unordered_set<std::wstring>& seen,
                    const std::filesystem::path& root, const Options& options) {
  std::vector<std::filesystem::path> pending{root};
  while (!pending.empty()) {
    if (stop_requested(options)) {
      result.cancelled = true;
      return;
    }
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
      if (stop_requested(options)) {
        result.cancelled = true;
        return;
      }
      if (ec) {
        ++result.inaccessible_count;
        ec.clear();
        continue;
      }
      const auto entry_path = it->path();
      const auto status = it->symlink_status(ec);
      if (ec) {
        ++result.inaccessible_count;
        ec.clear();
        continue;
      }
      if (std::filesystem::is_directory(status)) {
        // 不跟随符号链接、junction 和其他 reparse point，避免循环和越界扫描。
        if (!is_reparse_or_symlink(entry_path)) {
          pending.push_back(entry_path);
        }
        continue;
      }
      if (std::filesystem::is_regular_file(status)) {
        append_file(result, seen, entry_path, root, options);
      }
    }
  }
}

}  // namespace

std::wstring normalized_path_key(const std::filesystem::path& path) {
  std::error_code ec;
  auto absolute = std::filesystem::absolute(path, ec);
  auto normalized = (ec ? path : absolute).lexically_normal().wstring();
#ifdef _WIN32
  std::ranges::transform(normalized, normalized.begin(), [](wchar_t ch) {
    return static_cast<wchar_t>(std::towlower(static_cast<wint_t>(ch)));
  });
#endif
  return normalized;
}

Result scan(const std::vector<Root>& roots, Origin /*origin*/, const Options& options) {
  Result result;
  std::unordered_set<std::wstring> seen;
  try {
    for (const auto& root : roots) {
      if (stop_requested(options)) {
        result.cancelled = true;
        break;
      }
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
          add_error(result, std::format("拒绝递归扫描符号链接或 reparse 文件夹: {}",
                                        root.path.string()));
          continue;
        }
        scan_directory(result, seen, root.path, options);
        if (result.cancelled) break;
      } else {
        append_file(result, seen, root.path, {}, options);
      }
    }
    std::ranges::sort(result.files, [](const File& left, const File& right) {
      const auto left_key = normalized_path_key(left.path);
      const auto right_key = normalized_path_key(right.path);
      return left_key == right_key ? left.path.native() < right.path.native()
                                   : left_key < right_key;
    });
  } catch (const std::bad_alloc&) {
    add_error(result, "导入扫描时内存不足。");
  } catch (const std::filesystem::filesystem_error& error) {
    add_error(result, std::format("导入扫描时文件系统访问失败: {}", error.what()));
  }
  return result;
}

std::string summary_text(const Result& result, std::size_t added,
                         std::size_t queue_duplicates) {
  if (added == 0) {
    if (!result.errors.empty()) return result.errors.front();
    if (result.files.empty()) {
      return std::format("没有可导入的图片{}{}。",
                         result.unsupported_count == 0
                             ? ""
                             : std::format("，{} 个格式/大小不支持",
                                           result.unsupported_count),
                         result.inaccessible_count == 0
                             ? ""
                             : std::format("，{} 个不可访问",
                                           result.inaccessible_count));
    }
    return std::format("没有新图片加入队列，{} 个重复项已跳过。",
                       queue_duplicates + result.duplicate_count);
  }
  return std::format(
      "已加入 {} 张图片{}{}{}。", added,
      queue_duplicates + result.duplicate_count == 0
          ? ""
          : std::format("，跳过 {} 个重复项",
                        queue_duplicates + result.duplicate_count),
      result.unsupported_count == 0
          ? ""
          : std::format("，{} 个格式/大小不支持", result.unsupported_count),
      result.inaccessible_count == 0
          ? ""
          : std::format("，{} 个不可访问", result.inaccessible_count));
}

struct Dispatcher::Impl {
  Impl(OptionsProvider options_provider, Completion completion)
      : options_provider(std::move(options_provider)),
        completion(std::move(completion)),
        worker([this](std::stop_token token) { run(token); }) {}

  void run(std::stop_token token) {
    while (!token.stop_requested()) {
      Request request;
      {
        std::unique_lock lock{mutex};
        cv.wait(lock, token, [&] { return !jobs.empty(); });
        if (token.stop_requested()) break;
        if (jobs.empty()) continue;
        request = std::move(jobs.front());
        jobs.pop_front();
      }

      auto scan_options = options_provider ? options_provider() : Options{};
      scan_options.stop_requested = [token] { return token.stop_requested(); };
      auto result = scan(request.roots, request.origin, scan_options);
      if (token.stop_requested()) break;
      if (completion) {
        completion(std::move(request), std::move(result));
      }
    }
  }

  OptionsProvider options_provider;
  Completion completion;
  std::mutex mutex;
  std::condition_variable_any cv;
  std::deque<Request> jobs;
  std::jthread worker;
};

Dispatcher::Dispatcher(OptionsProvider options_provider, Completion completion)
    : impl_(std::make_unique<Impl>(std::move(options_provider),
                                   std::move(completion))) {}

Dispatcher::~Dispatcher() {
  request_stop();
  join();
}

Dispatcher::Dispatcher(Dispatcher&&) noexcept = default;
Dispatcher& Dispatcher::operator=(Dispatcher&&) noexcept = default;

bool Dispatcher::enqueue(Request request) {
  if (!impl_ || impl_->worker.get_stop_token().stop_requested()) return false;
  {
    std::scoped_lock lock{impl_->mutex};
    if (impl_->worker.get_stop_token().stop_requested()) return false;
    impl_->jobs.push_back(std::move(request));
  }
  impl_->cv.notify_one();
  return true;
}

void Dispatcher::request_stop() {
  if (!impl_) return;
  impl_->worker.request_stop();
  impl_->cv.notify_all();
}

void Dispatcher::join() {
  if (impl_ && impl_->worker.joinable()) {
    impl_->worker.join();
  }
}

bool Dispatcher::joinable() const noexcept {
  return impl_ && impl_->worker.joinable();
}

}  // namespace awj::ui_import
