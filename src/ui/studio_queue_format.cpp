#include "studio_queue_format.h"

#include <algorithm>
#include <random>
#include <format>
#include <string>

namespace awj::studio {

std::string queue_status_label(QueueItemStatus status) {
  switch (status) {
    case QueueItemStatus::running:
      return "正在编码";
    case QueueItemStatus::done:
      return "完成";
    case QueueItemStatus::failed:
      return "失败";
    case QueueItemStatus::skipped:
      return "已跳过";
    case QueueItemStatus::canceled:
      return "已取消";
    case QueueItemStatus::pending:
    default:
      return "等待编码";
  }
}

int queue_status_code(QueueItemStatus status) noexcept {
  switch (status) {
    case QueueItemStatus::running:
      return 1;
    case QueueItemStatus::done:
    case QueueItemStatus::skipped:
      return 2;
    case QueueItemStatus::failed:
      return 3;
    case QueueItemStatus::canceled:
      return 4;
    case QueueItemStatus::pending:
    default:
      return 0;
  }
}

std::string stage_seconds_text(double seconds) {
  return seconds < 0.0 ? std::string{"-"} : std::format("{:.3f}s", seconds);
}

std::string stage_timings_text(double decode_seconds, double prepare_seconds,
                               double encode_seconds, double write_seconds) {
  if (decode_seconds < 0.0 && prepare_seconds < 0.0 &&
      encode_seconds < 0.0 && write_seconds < 0.0) {
    return {};
  }
  return std::format("decode {} · prepare {} · encode {} · write {}",
                     stage_seconds_text(decode_seconds),
                     stage_seconds_text(prepare_seconds),
                     stage_seconds_text(encode_seconds),
                     stage_seconds_text(write_seconds));
}

bool queue_item_editable(const QueueImageItem& item) noexcept {
  return item.status == QueueItemStatus::pending;
}

bool queue_item_runnable(const QueueImageItem& item) noexcept {
  return item.status == QueueItemStatus::pending ||
         item.status == QueueItemStatus::failed ||
         item.status == QueueItemStatus::canceled;
}

bool queue_item_selected_for_run(const QueueImageItem& item,
                                 bool failed_only) noexcept {
  return failed_only ? item.status == QueueItemStatus::failed
                     : queue_item_runnable(item);
}

std::expected<std::vector<awj::ImageFile>, std::string> build_run_files(
    const awj::AppConfig& cfg, const std::vector<QueueImageItem>& queue,
    bool failed_only) {
  try {
    std::vector<awj::ImageFile> files;
    files.reserve(std::ranges::count_if(
        queue, [failed_only](const QueueImageItem& item) {
          return queue_item_selected_for_run(item, failed_only);
        }));
    std::random_device random_device;
    std::mt19937_64 rng{random_device()};
    const bool needs_hash =
        cfg.output_template.contains(L"{hash}") ||
        cfg.output_template.contains(L"{hash8}");
    const bool needs_sha256 =
        cfg.output_template.contains(L"{sha256}") ||
        cfg.output_template.contains(L"{sha2568}") ||
        cfg.output_template.contains(L"{sha256_8}");
    for (const auto& item : queue) {
      if (!queue_item_selected_for_run(item, failed_only)) {
        continue;
      }
      std::wstring hash;
      std::wstring sha256;
      if (needs_hash) {
        if (auto ok = awj::file_hash_token(item.path, hash); !ok) {
          return std::unexpected{ok.error()};
        }
      }
      if (needs_sha256) {
        if (auto ok = awj::file_sha256_token(item.path, sha256); !ok) {
          return std::unexpected{ok.error()};
        }
      }
      files.push_back(awj::make_image_file(files.size(), item.path,
                                           item.relative_dir,
                                           item.bytes, rng,
                                           std::move(hash),
                                           std::move(sha256)));
    }
    if (auto disambiguated = awj::apply_source_extension_disambiguation(cfg, files);
        !disambiguated) {
      return std::unexpected{disambiguated.error()};
    }
    if (auto resolved = awj::resolve_batch_output_paths(cfg, files); !resolved) {
      return std::unexpected{resolved.error()};
    }
    return files;
  } catch (const std::bad_alloc&) {
    return std::unexpected{"构建队列运行快照时内存不足。"};
  } catch (const std::length_error&) {
    return std::unexpected{"构建队列运行快照时数据超过运行时限制。"};
  } catch (const std::filesystem::filesystem_error&) {
    return std::unexpected{"构建队列运行快照时文件系统访问失败。"};
  }
}

}  // namespace awj::studio
