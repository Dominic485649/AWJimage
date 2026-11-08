#include "../src/ui/queue_ui_policy.h"
#include "../src/ui/studio_worker_events.h"

#include <array>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace {

void check(bool ok, const char* message) {
  if (!ok) throw std::runtime_error{message};
}

void options_check() {
  using namespace awj::studio;
  constexpr auto defaults = queue_options_from_config();
  static_assert(defaults.collision_index == 4 && !defaults.write_log &&
                !defaults.delete_source);
  constexpr std::array<std::wstring_view, 5> arguments{
      L"overwrite", L"skip", L"time", L"random", L"number"};
  for (int index = 0; index < static_cast<int>(arguments.size()); ++index) {
    const auto restored = queue_options_from_config(index);
    check(restored.collision_index == index &&
          queue_collision_argument(index) == arguments[index],
          "persisted collision index changed meaning");
  }
  check(queue_collision_argument(-1) == L"number" &&
        queue_options_from_config(5).collision_index == 4,
        "unknown collision index did not use safe default");
  for (const auto log : {std::optional<bool>{}, std::optional{false}, std::optional{true}}) {
    for (const auto summary : {std::optional<bool>{}, std::optional{false}, std::optional{true}}) {
      const auto migrated = queue_options_from_config({}, log, summary);
      check(migrated.write_log == (log.value_or(false) || summary.value_or(false)) &&
            !migrated.delete_source,
            "legacy logging migration enabled recycling or lost a report flag");
      check(queue_options_from_config({}, log, summary, false).delete_source == false &&
            queue_options_from_config({}, log, summary, true).delete_source,
            "explicit recycling choice was not independent of logging");
    }
  }
}

void open_target_check() {
  using namespace awj::studio;
  namespace fs = std::filesystem;
  const fs::path source{u8"queue/source.png"};
  const fs::path final{u8"queue/source(7).avif.png"};
  check(queue_image_open_target(source, final, false) == source,
        "pending, running, failed, canceled or skipped item opened a candidate");
  check(queue_image_open_target(source, final, true) == final,
        "success did not use the actual numbered output independently of its source");
  const fs::path absent;
  check(queue_image_open_target(source, absent, true).empty(),
        "success without a final output did not fail closed");
  check(queue_image_open_target(source, absent, false) == source,
        "reimport did not restore the source target");
}

void pointer_reset_check() {
  using awj::studio::queue_pointer_resets_double_click;
  static_assert(!queue_pointer_resets_double_click(2, 2),
                "Slint Other/Move must preserve the first click");
  for (int button = 0; button <= 2; ++button) {
    check(!queue_pointer_resets_double_click(button, 2),
          "ordinary pointer movement reset double-click tracking");
    for (const int kind : {0, 1})
      check(queue_pointer_resets_double_click(button, kind) == (button != 0),
            "only real non-left button presses/releases may reset click tracking");
    for (const int kind : {3, 4})
      check(queue_pointer_resets_double_click(button, kind),
            "cancellation or actual dragging retained a previous click");
  }
}

void output_event_check() {
  using namespace awj::studio;
  const std::string path = "queue/" + std::string{"\xe4\xb8\xad\xe6\x96\x87"} +
                           " name(7)\n.avif.png";
  constexpr std::string_view digits = "0123456789abcdef";
  std::string frame = "@AWJ-STUDIO/1 OUTPUT 12 ";
  for (const unsigned char byte : path) {
    frame.push_back(digits[byte >> 4]);
    frame.push_back(digits[byte & 15]);
  }
  const auto output = parse_studio_worker_output_event(frame);
  check(output && output->index == 12 && output->path_utf8 == path,
        "final output event lost numbering, Unicode, spaces or newline bytes");
  for (const auto invalid : {
      "@AWJ-STUDIO/1 OUTPUT 0 ", "@AWJ-STUDIO/1 OUTPUT 0 0",
      "@AWJ-STUDIO/1 OUTPUT -1 61", "@AWJ-STUDIO/1 OUTPUT 1 6g",
      "@AWJ-STUDIO/1 OUTPUT 1 00", "@AWJ-STUDIO/1 OUTPUT 1 6162 ",
      "@AWJ-STUDIO/1 OUTPUT 999999999999999999999999999999999 61"}) {
    check(!parse_studio_worker_output_event(invalid), "invalid output frame was accepted");
  }
}

}  // namespace

int main() try {
  options_check();
  open_target_check();
  pointer_reset_check();
  output_event_check();
  std::puts("queue-ui-policy: defaults, legacy migration, open target, pointer reset and OUTPUT framing passed");
  return 0;
} catch (const std::exception& error) {
  std::fprintf(stderr, "%s\n", error.what());
  return 1;
}
