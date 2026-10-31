#include "../src/codecs/avif_worker_failure.h"
#include <array>
#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

// 分配故障另由 codec_worker_failure_tests 验证；本夹具让 TSan 保留自己的分配器。
int main() {
  using awj::avif_worker::GridTileFailure;
  using awj::avif_worker::GridTileFailureState;
  for (int attempt = 0; attempt < 64; ++attempt) {
    GridTileFailureState state;
    std::array<std::string, 4> messages;
    for (auto& message : messages) message.assign(4096, 'x');
    std::atomic<bool> start{};
    std::vector<std::thread> workers;
    for (std::size_t i = 0; i < messages.size(); ++i) {
      workers.emplace_back([&, i] {
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        if (i & 1u) state.capture(messages[i]);
        else state.record(GridTileFailure::memory);
      });
    }
    start.store(true, std::memory_order_release);
    for (auto& worker : workers) worker.join();
    const auto error = state.take_error();
    if (state.reason() == GridTileFailure::tile) {
      if (error != std::string(4096, 'x')) return 1;
    } else if (state.reason() != GridTileFailure::memory || !error.empty()) return 1;
  }
  std::puts("AVIF 首失败状态与 join 后错误发布竞争夹具通过。");
  return 0;
}
