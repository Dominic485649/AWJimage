#include "../src/codecs/avif_worker_failure.h"
#include <array>
#include <cstdlib>
#include <cstdio>
#include <new>
#include <thread>
#include <vector>

namespace {
thread_local bool deny_new = false;
std::atomic<unsigned> denied{};
struct DenyAllocation {
  DenyAllocation() noexcept { deny_new = true; }
  ~DenyAllocation() { deny_new = false; }
};
}

void* operator new(std::size_t size) {
  if (deny_new) { denied.fetch_add(1); throw std::bad_alloc{}; }
  if (auto* pointer = std::malloc(size ? size : 1)) return pointer;
  throw std::bad_alloc{};
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }

#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "第 %d 行 worker 故障契约失败：%s\n", __LINE__, #value); return 1; } } while (false)
int main() {
  using awj::avif_worker::GridTileFailure;
  using awj::avif_worker::GridTileFailureState;
  {
    GridTileFailureState state;
    std::thread worker([&] {
      DenyAllocation guard;
      try {
        auto* pointer = ::operator new(32);
        ::operator delete(pointer);
      } catch (const std::bad_alloc&) {
        state.record(GridTileFailure::memory);
      }
    });
    worker.join();
    CHECK(denied.load() == 1 && state.reason() == GridTileFailure::memory);
    CHECK(state.take_error().empty());
  }
  {
    GridTileFailureState state;
    std::string error(8192, 'x');
    const auto before = denied.load();
    std::thread worker([&] { DenyAllocation guard; state.capture(error); });
    worker.join();
    auto result = state.take_error();
    CHECK(denied.load() == before && result == std::string(8192, 'x'));
    CHECK(state.reason() == GridTileFailure::tile && error.empty());
  }
  for (int attempt = 0; attempt < 32; ++attempt) {
    GridTileFailureState state;
    std::array<std::string, 4> errors;
    for (auto& error : errors) error.assign(4096, 'y');
    std::atomic<bool> start{};
    std::vector<std::thread> workers;
    const auto before = denied.load();
    for (std::size_t i = 0; i < errors.size(); ++i) {
      workers.emplace_back([&, i] {
        DenyAllocation guard;
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        if (i & 1u) state.capture(errors[i]);
        else state.record(GridTileFailure::unknown);
      });
    }
    start.store(true, std::memory_order_release);
    for (auto& worker : workers) worker.join();
    auto result = state.take_error();
    CHECK(denied.load() == before);
    CHECK(state.reason() == GridTileFailure::tile ? result == std::string(4096, 'y') :
          state.reason() == GridTileFailure::unknown && result.empty());
  }
  std::puts("实际 AVIF 错误状态在 bad_alloc、长错误文本和首失败竞争下无二次分配。");
  return 0;
}
