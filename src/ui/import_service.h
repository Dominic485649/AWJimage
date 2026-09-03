#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace awj::ui_import {

enum class Origin {
  file_dialog,
  folder_dialog,
  drag_drop,
  command_line,
};

struct Root {
  std::filesystem::path path;
  bool force_directory{};
};

struct Request {
  std::vector<Root> roots;
  Origin origin{Origin::drag_drop};
  std::filesystem::path input_hint;
  bool update_input_path{true};
};

struct File {
  std::filesystem::path path;
  std::filesystem::path source_root;
  std::uintmax_t bytes{};
};

struct Result {
  std::vector<File> files;
  std::size_t duplicate_count{};
  std::size_t unsupported_count{};
  std::size_t inaccessible_count{};
  std::vector<std::string> errors;
  bool cancelled{};
};

using SupportedPredicate = std::function<bool(const std::filesystem::path&)>;
using StopPredicate = std::function<bool()>;

struct Options {
  SupportedPredicate is_supported;
  StopPredicate stop_requested;
  std::uintmax_t maximum_file_bytes{};
};

class Dispatcher {
 public:
  using Completion = std::function<void(Request, Result)>;
  using OptionsProvider = std::function<Options()>;

  Dispatcher(OptionsProvider options_provider, Completion completion);
  ~Dispatcher();
  Dispatcher(Dispatcher&&) noexcept;
  Dispatcher& operator=(Dispatcher&&) noexcept;
  Dispatcher(const Dispatcher&) = delete;
  Dispatcher& operator=(const Dispatcher&) = delete;

  bool enqueue(Request request);
  void request_stop();
  void join();
  [[nodiscard]] bool joinable() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// 统一处理来自文件选择器、文件夹选择器、拖放与命令行的输入根。
// 扫描阶段只做文件系统工作，不依赖 UI，也不修改队列；调用方可以安全地放到后台线程。
Result scan(const std::vector<Root>& roots, Origin origin, const Options& options);

std::string summary_text(const Result& result, std::size_t added,
                         std::size_t queue_duplicates);

// 与队列去重使用同一类“绝对 + 规范化 + Windows 大小写不敏感”键。
std::wstring normalized_path_key(const std::filesystem::path& path);

}  // namespace awj::ui_import
