#include "../src/compat/stdlib_compat.h"
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <zlib.h>

import awj.config;
import awj.core;
import awj.decoder_common;
import awj.decoder_registry;
import awj.image;
import awj.large_image_plan;
import awj.pipeline;
import awj.png_codec;

namespace {
constexpr std::uint64_t mib = 1024ull * 1024;
int fail(std::string_view message) {
  std::fprintf(stderr, "%.*s\n", static_cast<int>(message.size()), message.data());
  return 1;
}
bool budget_error(const auto& result) {
  return !result && result.error().starts_with("尺寸探测工作集 ");
}
bool write(const std::filesystem::path& path, std::span<const std::byte> bytes) {
  std::ofstream out{path, std::ios::binary};
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  return static_cast<bool>(out);
}
std::vector<std::byte> png_header() {
  std::vector<std::byte> result{std::byte{137}, std::byte{'P'}, std::byte{'N'}, std::byte{'G'},
      std::byte{13}, std::byte{10}, std::byte{26}, std::byte{10}};
  std::array<std::byte, 13> ihdr{};
  ihdr[3] = ihdr[7] = std::byte{1};
  ihdr[8] = std::byte{8}; ihdr[9] = std::byte{6};
  awj::png_detail::append_png_chunk(result, "IHDR", ihdr);
  return result;
}
}

int main() {
  const auto root = std::filesystem::path{AWJ_TEST_RESULTS_ROOT} /
      ("probe-memory-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(root);
  struct Cleanup {
    std::filesystem::path path;
    bool passed{};
    ~Cleanup() { if (passed) { std::error_code ec; std::filesystem::remove_all(path, ec); } }
  } cleanup{root};
  const auto tiny = root / "tiny.bin";
  const std::array two{std::byte{1}, std::byte{2}};
  if (!write(tiny, two)) return fail("无法建立读取额度测试输入。");
  {
    const awj::decoder_common::ScopedProbeFileReadLimit outer{1};
    const awj::decoder_common::ScopedProbeFileReadLimit inner{UINT64_MAX};
    if (!budget_error(awj::decoder_common::read_file_bytes(tiny, "测试")))
      return fail("整输入读取未在缓冲分配前执行额度检查，或嵌套作用域放宽了额度。");
  }
  if (!awj::decoder_common::read_file_bytes(tiny, "测试"))
    return fail("预检读取额度泄漏到后续普通调用。");

  const auto renamed = root / "renamed.png";
  {
    std::ofstream out{renamed, std::ios::binary};
    const std::array<unsigned char, 10> marker{0xff, 0xd8, 0xff, 0xe2, 0, 6, 'M', 'P', 'F', 0};
    out.write(reinterpret_cast<const char*>(marker.data()), marker.size());
    out.seekp(static_cast<std::streamoff>(64 * mib - 1)); out.put('\0');
    if (!out) return fail("无法建立改后缀 Gain Map 输入。");
  }
  if (!budget_error(awj::probe_image_dimensions_for_path(renamed,
      {.allow_wic_fallback = false, .probe_memory_reservation_bytes = 8 * mib})))
    return fail("PNG 后缀绕过了 Gain Map 整输入预约。");
  auto cfg = awj::default_app_config(); cfg.max_jobs = 8;
  const std::vector<awj::ImageFile> renamed_files{{.path = renamed, .bytes = 64 * mib}};
  const auto classified = awj::pipeline_detail::classify_work_for_avif(cfg, renamed_files, 16 * mib);
  if (!classified || classified->memory_rejected.size() != 1 || !classified->ordinary.empty())
    return fail("分类器启动了超过总预算的改后缀整输入探测。");

  auto header = png_header();
  awj::png_detail::append_be_u32(header, 20 * mib);
  for (const char ch : std::string_view{"iCCP"}) header.push_back(static_cast<std::byte>(ch));
  bool payload_read{};
  const auto read = [&](std::uint64_t offset, std::size_t count)
      -> awj::compat::expected<std::span<const std::byte>, std::string> {
    if (offset > header.size() || count > header.size() - offset) {
      payload_read = true;
      return awj::compat::unexpected{"不应读取未获预约的 metadata 数据。"};
    }
    return std::span<const std::byte>{header}.subspan(static_cast<std::size_t>(offset), count);
  };
  const auto visit = [](std::string_view, std::span<const std::byte>, std::uint32_t, std::uint64_t)
      -> awj::compat::expected<void, std::string> { return {}; };
  const auto large_chunk = awj::png_detail::scan_png_chunks(33 + 12 + 20 * mib, read, visit,
      "oversized-iCCP", true, {}, 8 * mib);
  if (!budget_error(large_chunk) || payload_read)
    return fail("PNG 大 iCCP 未在分配和读取数据之前拒绝。");

  std::vector<std::byte> expanded(4 * mib);
  std::vector<std::byte> compressed(compressBound(static_cast<uLong>(expanded.size())));
  uLongf length = static_cast<uLongf>(compressed.size());
  if (compress2(reinterpret_cast<Bytef*>(compressed.data()), &length,
      reinterpret_cast<const Bytef*>(expanded.data()), static_cast<uLong>(expanded.size()), 9) != Z_OK)
    return fail("无法生成 ICC 展开边界输入。");
  std::vector<std::byte> payload{std::byte{'p'}, std::byte{0}, std::byte{0}};
  payload.insert(payload.end(), compressed.begin(), compressed.begin() + length);
  if (!budget_error(awj::png_detail::inspect_icc_dynamic_range(payload, 8 * mib)))
    return fail("PNG ICC 展开越过了压缩块与 profile 的联合预约。");
  auto unrestricted = awj::png_detail::inspect_icc_dynamic_range(payload);
  if (!unrestricted || *unrestricted != awj::ImageDynamicRangeHint::unknown)
    return fail("无法确认的 ICC 被当成 SDR，或未提供预约时改变了原有语义。");
  const auto policy_payload = [] {
    std::vector<std::byte> raw(65 * mib);
    std::vector<std::byte> output(compressBound(static_cast<uLong>(raw.size())));
    uLongf bytes = static_cast<uLongf>(output.size());
    if (compress2(reinterpret_cast<Bytef*>(output.data()), &bytes,
        reinterpret_cast<const Bytef*>(raw.data()), static_cast<uLong>(raw.size()), 9) != Z_OK)
      return std::vector<std::byte>{};
    std::vector<std::byte> result{std::byte{'p'}, std::byte{0}, std::byte{0}};
    result.insert(result.end(), output.begin(), output.begin() + bytes);
    return result;
  }();
  if (policy_payload.empty()) return fail("无法生成 metadata 策略边界输入。");
  const auto policy_rejected = awj::png_detail::inspect_icc_dynamic_range(policy_payload, 512 * mib);
  if (policy_rejected || budget_error(policy_rejected) || !policy_rejected.error().starts_with("PNG ICC profile "))
    return fail("独立 metadata 策略上限被误报为工作集预约不足。");
  awj::compat::stop_source stop; stop.request_stop();
  if (awj::png_detail::inspect_icc_dynamic_range(payload, 8 * mib, stop.get_token()))
    return fail("PNG ICC 预检忽略入口取消。");

  auto png = png_header();
  awj::png_detail::append_png_chunk(png, "iCCP", payload);
  const std::array invalid_pixels{std::byte{0}};
  awj::png_detail::append_png_chunk(png, "IDAT", invalid_pixels);
  awj::png_detail::append_png_chunk(png, "IEND", {});
  const auto expanded_path = root / "expanded-profile.png";
  if (!write(expanded_path, png)) return fail("无法写入联合额度 PNG 输入。");
  if (!budget_error(awj::probe_image_dimensions_for_path(expanded_path,
      {.allow_wic_fallback = true, .probe_memory_reservation_bytes = 8 * mib})))
    return fail("registry 未把 PNG 预检额度传至解压缓冲，或经 WIC 兜底绕过了限制。");
  const std::vector<awj::ImageFile> png_files{{.path = expanded_path, .bytes = png.size()}};
  const auto retried = awj::pipeline_detail::classify_work_for_avif(cfg, png_files, 16 * mib);
  if (!retried || retried->ordinary.size() != 1 || !retried->memory_rejected.empty() ||
      retried->ordinary[0].dimensions.source_dynamic_range != awj::ImageDynamicRangeHint::unknown)
    return fail("并行预检释放预约后的单次独占重试未安全精化未知 ICC。");
  cleanup.passed = true;
  return 0;
}
