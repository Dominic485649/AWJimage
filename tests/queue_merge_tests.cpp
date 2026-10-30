#include "../src/ui/queue_merge.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void check(bool value, const char* message) {
  if (!value) throw std::runtime_error{message};
}

awj::ui_queue::MergePlan merge(const std::vector<std::filesystem::path>& previous,
                               const std::vector<std::filesystem::path>& incoming) {
  return awj::ui_queue::plan_merge(previous.size(), incoming.size(),
      [&](std::size_t i) { return awj::ui_queue::path_key(previous[i]); },
      [&](std::size_t i) { return awj::ui_queue::path_key(incoming[i]); });
}
}

int main() try {
  using namespace awj::ui_queue;
  const auto plan = merge({"A.png", "B.png"}, {"C.png", "A.png", "C.png"});
  check(plan.added == 1 && plan.requeued == 1 && plan.folded == 1 && plan.entries.size() == 3,
        "混合状态队列的合并数量错误");
  check(plan.entries[0].previous == 1 && plan.entries[0].incoming == no_index &&
        plan.entries[1].previous == 0 && plan.entries[1].incoming == 1 &&
        plan.entries[2].previous == no_index && plan.entries[2].incoming == 2,
        "未命中旧项被重排，或末次位置不正确");
  const auto ordered = merge({}, {"C.png", "A.png", "C.png", "B.png"});
  check(ordered.entries.size() == 3 && ordered.entries[0].incoming == 1 &&
        ordered.entries[1].incoming == 2 && ordered.entries[2].incoming == 3,
        "[C,A,C,B] 必须折叠为 [A,C,B]");
  const auto aliases = merge({"folder/../A.png"}, {"./A.png", "B.png", "x/../A.png"});
  check(aliases.entries.size() == 2 && aliases.entries[0].incoming == 1 &&
        aliases.entries[1].previous == 0 && aliases.entries[1].incoming == 2,
        "词法路径别名没有以末次有效位置合并");
  const auto empty = merge({"A.png", "B.png"}, {});
  check(empty.entries.size() == 2 && empty.entries[0].previous == 0 &&
        empty.entries[1].previous == 1 && !empty.added && !empty.requeued,
        "空导入改变了旧队列");
#ifdef _WIN32
  check(merge({"A.PNG"}, {"a.png"}).requeued == 1, "Windows 路径大小写没有归一");
#else
  check(merge({"A.PNG"}, {"a.png"}).added == 1, "Linux 路径大小写被错误折叠");
  check(path_key(std::filesystem::path{u8"中文/图片.png"}).find(".png") != std::string::npos,
        "Linux 原生 UTF-8 路径失败");
#endif
  std::vector<std::filesystem::path> previous, incoming;
  constexpr std::size_t count = 50000;
  previous.reserve(count);
  incoming.reserve(count * 2);
  for (std::size_t i = 0; i < count; ++i) previous.emplace_back(std::to_string(i) + ".png");
  for (std::size_t i = count; i > 0; --i) incoming.push_back(previous[i - 1]);
  for (std::size_t i = 0; i < count; ++i) incoming.push_back(previous[i]);
  const auto start = std::chrono::steady_clock::now();
  const auto large = merge(previous, incoming);
  check(large.entries.size() == count && large.requeued == count &&
        large.folded == count && !large.added, "大批量重复合并违反唯一性");
  for (std::size_t i = 0; i < count; ++i)
    check(large.entries[i].previous == i && large.entries[i].incoming == count + i,
          "大批量合并丢失末次位置或稳定身份");
  std::printf("queue merge passed: %zu old / %zu incoming, %.3f s\n", previous.size(), incoming.size(),
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
  return 0;
} catch (const std::exception& error) {
  std::fprintf(stderr, "%s\n", error.what());
  return 1;
}
