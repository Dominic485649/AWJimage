#include "../src/ui/studio_worker_events.h"

#include <array>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void check(bool ok, const char* message) {
  if (!ok) throw std::runtime_error{message};
}

void resize_event_check() {
  using namespace awj::studio;
  const StudioWorkerResizeEvent defaults;
  check(defaults.resize_microseconds == -1 && defaults.resize_workers == 0,
        "default resize event must be unknown");
  const auto unknown = parse_studio_worker_resize_event("@AWJ-STUDIO/1 RESIZE 0 -1 0");
  check(unknown && unknown->index == 0 && unknown->resize_microseconds == -1 &&
        unknown->resize_workers == 0, "unknown resize was not preserved");
  const auto noop = parse_studio_worker_resize_event("@AWJ-STUDIO/1 RESIZE 12 0 0");
  check(noop && noop->index == 12 && noop->resize_microseconds == 0 &&
        noop->resize_workers == 0, "confirmed no-transform was not preserved");
  const auto parallel = parse_studio_worker_resize_event("@AWJ-STUDIO/1 RESIZE 12 123456 8");
  check(parallel && parallel->resize_microseconds == 123456 && parallel->resize_workers == 8,
        "resize duration or actual workers changed");
  const auto partial = parse_studio_worker_resize_event("@AWJ-STUDIO/1 RESIZE 12 123456 0");
  check(partial && partial->resize_microseconds == 123456 && partial->resize_workers == 0,
        "partial failure timing with zero workers was lost");
  const auto rounded = parse_studio_worker_resize_event("@AWJ-STUDIO/1 RESIZE 12 0 1");
  check(rounded && rounded->resize_workers == 1,
        "sub-microsecond work rounded to zero must remain representable");

  const auto index_max = std::to_string(std::numeric_limits<std::size_t>::max());
  const auto elapsed_max = std::to_string(std::numeric_limits<std::int64_t>::max());
  const auto workers_max = std::to_string(std::numeric_limits<int>::max());
  const auto maximum = parse_studio_worker_resize_event(
      "@AWJ-STUDIO/1 RESIZE " + index_max + " " + elapsed_max + " " + workers_max);
  check(maximum && maximum->index == std::numeric_limits<std::size_t>::max() &&
        maximum->resize_microseconds == std::numeric_limits<std::int64_t>::max() &&
        maximum->resize_workers == std::numeric_limits<int>::max(),
        "representable resize boundaries were rejected");
  // 队列边界由消费者校验；解析器不知道队列大小。
  for (const auto& invalid : {
           "@AWJ-STUDIO/1 RESIZE " + index_max + "0 1 1",
           "@AWJ-STUDIO/1 RESIZE 0 " + elapsed_max + "0 1",
           "@AWJ-STUDIO/1 RESIZE 0 1 " + workers_max + "0"}) {
    check(!parse_studio_worker_resize_event(invalid), "resize overflow was accepted");
  }
  for (const std::string_view invalid : {
           "", "RESIZE 0 1 1", "@AWJ-STUDIO/2 RESIZE 0 1 1",
           "@AWJ-STUDIO/1 RESIZEX 0 1 1", "@AWJ-STUDIO/1 RESIZE",
           "@AWJ-STUDIO/1 RESIZE ", "@AWJ-STUDIO/1 RESIZE 0",
           "@AWJ-STUDIO/1 RESIZE 0 1", "@AWJ-STUDIO/1 RESIZE 0 1 ",
           "@AWJ-STUDIO/1 RESIZE  0 1 1", "@AWJ-STUDIO/1 RESIZE 0  1 1",
           "@AWJ-STUDIO/1 RESIZE 0 1  1", "@AWJ-STUDIO/1 RESIZE 0 1 1 ",
           "@AWJ-STUDIO/1 RESIZE 0 1 1 extra", "@AWJ-STUDIO/1 RESIZE 0 1 1\n",
           "@AWJ-STUDIO/1 RESIZE 0 1 1\r", "@AWJ-STUDIO/1 RESIZE 0 1\t1",
           "@AWJ-STUDIO/1 RESIZE -1 1 1", "@AWJ-STUDIO/1 RESIZE +1 1 1",
           "@AWJ-STUDIO/1 RESIZE 1x 1 1", "@AWJ-STUDIO/1 RESIZE 0 -2 0",
           "@AWJ-STUDIO/1 RESIZE 0 -9223372036854775809 0",
           "@AWJ-STUDIO/1 RESIZE 0 +1 1", "@AWJ-STUDIO/1 RESIZE 0 1x 1",
           "@AWJ-STUDIO/1 RESIZE 0 1.5 1", "@AWJ-STUDIO/1 RESIZE 0 1e6 1",
           "@AWJ-STUDIO/1 RESIZE 0 nan 0", "@AWJ-STUDIO/1 RESIZE 0 inf 0",
           "@AWJ-STUDIO/1 RESIZE 0 -1 1", "@AWJ-STUDIO/1 RESIZE 0 1 -1",
           "@AWJ-STUDIO/1 RESIZE 0 1 +1", "@AWJ-STUDIO/1 RESIZE 0 1 1x"}) {
    check(!parse_studio_worker_resize_event(invalid), "malformed resize event was accepted");
  }
  std::string embedded_nul = "@AWJ-STUDIO/1 RESIZE 0 1 1";
  embedded_nul.push_back('\0');
  embedded_nul += "ignored";
  check(!parse_studio_worker_resize_event(embedded_nul),
        "resize parser did not consume an embedded NUL suffix");
  check(is_studio_worker_resize_event_line("@AWJ-STUDIO/1 RESIZE") &&
        is_studio_worker_resize_event_line("@AWJ-STUDIO/1 RESIZE invalid") &&
        !is_studio_worker_resize_event_line("@AWJ-STUDIO/1 RESIZEX 0 1 1") &&
        !is_studio_worker_resize_event_line("@AWJ-STUDIO/2 RESIZE 0 1 1"),
        "rejected resize frames must still be distinguishable from human logs");
}

void legacy_detail_check() {
  using namespace awj::studio;
  constexpr std::string_view line = "@AWJ-STUDIO/1 DETAIL 12 aom 2 123 -1 456789 0";
  const auto detail = parse_studio_worker_detail_event(line);
  check(detail && detail->index == 12 && detail->encoder_id == "aom" &&
        detail->encoder_threads == 2 && detail->decode_microseconds == 123 &&
        detail->prepare_microseconds == -1 && detail->encode_microseconds == 456789 &&
        detail->write_microseconds == 0, "legacy DETAIL layout or unknown timing changed");
  const auto unknown = parse_studio_worker_detail_event("@AWJ-STUDIO/1 DETAIL 0 - 0 -1 -1 -1 -1");
  check(unknown && unknown->encoder_id.empty() && unknown->encoder_threads == 0 &&
        unknown->write_microseconds == -1, "legacy unknown DETAIL was not preserved");
  check(!parse_studio_worker_resize_event(line) &&
        !parse_studio_worker_detail_event("@AWJ-STUDIO/1 RESIZE 0 1 1"),
        "DETAIL and RESIZE must remain independent event formats");
  for (const std::string_view invalid : {
           "@AWJ-STUDIO/1 DETAIL", "@AWJ-STUDIO/1 DETAIL 0 aom 2 1 2 3",
           "@AWJ-STUDIO/1 DETAIL 0 aom 2 1 2 3 4 5",
           "@AWJ-STUDIO/1 DETAIL 0  2 1 2 3 4",
           "@AWJ-STUDIO/1 DETAIL -1 aom 2 1 2 3 4",
           "@AWJ-STUDIO/1 DETAIL 0 aom -1 1 2 3 4",
           "@AWJ-STUDIO/1 DETAIL 0 aom 2 -2 2 3 4",
           "@AWJ-STUDIO/1 DETAIL 0 aom 2 1 -2 3 4",
           "@AWJ-STUDIO/1 DETAIL 0 aom 2 1 2 -2 4",
           "@AWJ-STUDIO/1 DETAIL 0 aom 2 1 2 3 -2",
           "@AWJ-STUDIO/1 DETAIL 0 aom 2 1 2 3 4x",
           "@AWJ-STUDIO/1 DETAIL 0 aom 2 1 2 3 4 extra",
           "@AWJ-STUDIO/1 DETAIL 999999999999999999999999 aom 2 1 2 3 4",
           "@AWJ-STUDIO/1 DETAIL 0 aom 999999999999999999999999 1 2 3 4",
           "@AWJ-STUDIO/1 DETAIL 0 aom 2 9223372036854775808 2 3 4",
           "@AWJ-STUDIO/1 DETAIL 0 aom 2 1 9223372036854775808 3 4",
           "@AWJ-STUDIO/1 DETAIL 0 aom 2 1 2 9223372036854775808 4",
           "@AWJ-STUDIO/1 DETAIL 0 aom 2 1 2 3 9223372036854775808"}) {
    check(!parse_studio_worker_detail_event(invalid), "invalid legacy DETAIL was accepted");
  }
}

void timing_merge_check() {
  using namespace awj::studio;
  const auto detail = parse_studio_worker_detail_event("@AWJ-STUDIO/1 DETAIL 0 aom 2 123 -1 456789 0");
  const auto resize = parse_studio_worker_resize_event("@AWJ-STUDIO/1 RESIZE 0 123456 8");
  check(detail && resize, "timing fixtures were rejected");
  StudioWorkerStageTimings detail_first, resize_first;
  check(detail_first.decode_seconds == -1.0 && detail_first.resize_seconds == -1.0 &&
        detail_first.resize_workers == 0, "initial stage timing was not unknown");
  detail_first.apply(*detail);
  check(detail_first.resize_seconds == -1.0 && detail_first.resize_workers == 0,
        "old worker without RESIZE must not synthesize zero");
  detail_first.apply(*resize);
  resize_first.apply(*resize);
  check(resize_first.decode_seconds == -1.0 && resize_first.prepare_seconds == -1.0,
        "resize-only event fabricated DETAIL timings");
  resize_first.apply(*detail);
  check(detail_first.decode_seconds == resize_first.decode_seconds &&
        detail_first.prepare_seconds == -1.0 && resize_first.prepare_seconds == -1.0 &&
        detail_first.encode_seconds == resize_first.encode_seconds &&
        detail_first.write_seconds == resize_first.write_seconds &&
        detail_first.resize_seconds == 0.123456 &&
        detail_first.resize_seconds == resize_first.resize_seconds &&
        detail_first.resize_workers == 8 && resize_first.resize_workers == 8,
        "DETAIL/RESIZE arrival order lost or rounded raw stage timings");
  resize_first.apply(StudioWorkerDetailEvent{});
  check(resize_first.resize_seconds == 0.123456 && resize_first.resize_workers == 8,
        "later DETAIL overwrote resize timing");
  detail_first.apply(StudioWorkerResizeEvent{});
  check(detail_first.resize_seconds == -1.0 && detail_first.resize_workers == 0 &&
        detail_first.decode_seconds == studio_worker_seconds(123) &&
        detail_first.encode_seconds == studio_worker_seconds(456789),
        "unknown RESIZE overwrote unrelated timing or retained stale workers");
  resize_first = StudioWorkerStageTimings{};
  check(resize_first.decode_seconds == -1.0 && resize_first.resize_seconds == -1.0 &&
        resize_first.resize_workers == 0, "new shell run retained previous resize");
  check(studio_worker_seconds(-1) == -1.0 && studio_worker_seconds(0) == 0.0 &&
        studio_worker_seconds(1) == 0.000001, "microsecond sentinel conversion changed");
}

void legacy_progress_check() {
  using namespace awj::studio;
  for (const char status : std::array{'R', 'D', 'S', 'C', 'F'}) {
    const auto item = parse_studio_worker_item_event(
        "@AWJ-STUDIO/1 ITEM 0 " + std::string(1, status) + " 1 1");
    check(item && item->index == 0 && item->status == status &&
          item->completed == 1 && item->total == 1, "legacy ITEM was not preserved");
  }
  for (const std::string_view invalid : {
           "@AWJ-STUDIO/1 ITEM 1 D 1 1", "@AWJ-STUDIO/1 ITEM 0 X 0 1",
           "@AWJ-STUDIO/1 ITEM 0 D 2 1", "@AWJ-STUDIO/1 ITEM 0 D 0 0",
           "@AWJ-STUDIO/1 ITEM -1 D 0 1", "@AWJ-STUDIO/1 ITEM 0 D -1 1",
           "@AWJ-STUDIO/1 ITEM 0 D 0 -1", "@AWJ-STUDIO/1 ITEM 0 D 1 1 extra"})
    check(!parse_studio_worker_item_event(invalid), "invalid ITEM was accepted");
  const auto frame = parse_studio_worker_frame_event("@AWJ-STUDIO/1 FRAME 12 1 16");
  check(frame && frame->index == 12 && frame->completed == 1 && frame->total == 16,
        "legacy FRAME was not preserved");
  check(!parse_studio_worker_frame_event("@AWJ-STUDIO/1 FRAME 0 0 16") &&
        !parse_studio_worker_frame_event("@AWJ-STUDIO/1 FRAME 0 17 16") &&
        !parse_studio_worker_frame_event("@AWJ-STUDIO/1 FRAME 0 1 16 extra"),
        "invalid FRAME was accepted");
  const auto output = parse_studio_worker_output_event("@AWJ-STUDIO/1 OUTPUT 12 612062");
  check(output && output->index == 12 && output->path_utf8 == "a b",
        "legacy OUTPUT was not preserved");
}

}  // namespace

int main() try {
  resize_event_check();
  legacy_detail_check();
  timing_merge_check();
  legacy_progress_check();
  return 0;
} catch (const std::exception& error) {
  std::fprintf(stderr, "%s\n", error.what());
  return 1;
}
