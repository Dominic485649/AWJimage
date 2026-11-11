#include "../src/core/work_memory_admission.h"
#include <array>
#include <chrono>
#include <cstdio>
#include <future>
#include <limits>
#include <stop_token>

int main() {
  const auto fail = [](const char* message) { std::fprintf(stderr, "%s\n", message); return 1; };
  struct Group { std::uint64_t estimated_bytes; bool exclusive_refinement{}; };
  {
    const std::array work{Group{20, true}, Group{3}, Group{2}};
    awj::WorkMemoryAdmission admission{10, work};
    if (admission.runnable_count(16) != 2) return fail("可运行快照没有按真实内存容纳任务。");
    auto exclusive = admission.acquire({});
    if (!exclusive || exclusive->index != 0 || exclusive->bytes != 10 || work[0].estimated_bytes != 20)
      return fail("独占精化没有保留原估算与独占预算语义。");
    if (admission.runnable_count(16) != 1) return fail("内存阻塞任务错误占用 CPU 可运行份额。");
    auto owned = exclusive->bytes;
    if (!admission.try_resize(owned, 6) || owned != 6) return fail("精化后预约无法缩小。");
    auto small = admission.acquire({});
    if (!small || small->bytes != 3) return fail("精化释放的容量无法被小任务回填。");
    if (admission.try_resize(owned, 8) || owned != 6) return fail("预约增长越过了全局预算。");
    std::stop_source cancel;
    auto waiting = std::async(std::launch::async, [&] { return admission.acquire(cancel.get_token()); });
    cancel.request_stop();
    if (waiting.wait_for(std::chrono::seconds{2}) != std::future_status::ready || waiting.get())
      return fail("预约等待不能响应取消。");
    admission.release(small->bytes);
    if (!admission.try_resize(owned, 8) || owned != 8) return fail("释放其他任务后不能非阻塞扩容。");
    admission.release(owned);
    auto final = admission.acquire({});
    if (!final || final->index != 2) return fail("取消等待丢失了排队任务。");
    admission.release(final->bytes);
    if (admission.acquire({})) return fail("同一任务被派发两次。");
  }
  {
    const auto max = std::numeric_limits<std::uint64_t>::max();
    const std::array work{Group{max - 1}, Group{1}};
    awj::WorkMemoryAdmission admission{max, work};
    auto a = admission.acquire({}); auto b = admission.acquire({});
    auto owned = a->bytes;
    if (admission.try_resize(owned, max) || owned != max - 1) return fail("极值扩容发生溢出。");
    admission.release(b->bytes);
    if (!admission.try_resize(owned, max)) return fail("极值合法扩容被拒绝。");
    admission.release(owned);
  }
  try {
    const std::array work{Group{11}};
    awj::WorkMemoryAdmission admission{10, work};
    return fail("未标识的超预算任务被静默夹到预算。");
  } catch (const std::invalid_argument&) {}
  {
    const std::array work{Group{20, true}};
    awj::WorkMemoryAdmission admission{0, work};
    auto ticket = admission.acquire({}); auto owned = ticket->bytes;
    if (!admission.try_resize(owned, 100) || owned) return fail("解除预算的预约语义改变。");
    admission.release(owned);
  }
  return 0;
}
