#include "../src/compat/stdlib_compat.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "第 %d 行兼容契约失败：%s\n", __LINE__, #value); return 1; } } while (false)

int main() {
  awj::compat::expected<std::unique_ptr<int>, std::string> owned{std::make_unique<int>(7)};
  auto moved = std::move(owned);
  CHECK(moved && **moved == 7);
  awj::compat::expected<void, std::string> success;
  CHECK(success);
  success = awj::compat::unexpected{std::string{"owned error"}};
  CHECK(!success && success.error() == "owned error");
  success = awj::compat::unexpected{std::string{"replacement"}};
  CHECK(!success && success.error() == "replacement");
  awj::compat::expected<int, std::string> error{awj::compat::unexpected{"契约错误"}};
  CHECK(!error && error.error() == "契约错误");
  bool threw = false;
  try { (void)error.value(); } catch (const std::exception&) { threw = true; }
  CHECK(threw);

  using namespace std::chrono;
  const auto epoch = sys_seconds{seconds{0}};
  const auto before = sys_seconds{seconds{-1}};
  CHECK(awj::compat::format(L"{:%Y%m%d-%H%M%S}", epoch) == L"19700101-000000");
  CHECK(awj::compat::format(L"{:%Y%m%d}", before) == L"19691231");
  CHECK(awj::compat::format(L"{:%H%M%S}", before) == L"235959");
  CHECK(awj::compat::format("{:08x}", 42) == "0000002a");
  CHECK(awj::compat::format("{:.3f}", 1.25) == "1.250");
  CHECK(awj::compat::format(L"{} {}", L"图像", 2) == L"图像 2");
  auto* output = std::tmpfile();
  CHECK(output);
  awj::compat::println(output, "value={}", 7);
  std::rewind(output);
  char text[16]{};
  CHECK(std::fread(text, 1, 8, output) == 8);
  CHECK(std::string(text, 8) == "value=7\n");
  CHECK(std::fclose(output) == 0);

  awj::compat::stop_token empty;
  CHECK(!empty.stop_possible() && !empty.stop_requested());
  awj::compat::stop_token survivor;
  {
    awj::compat::stop_source source;
    survivor = source.get_token();
    CHECK(survivor.stop_possible());
    auto copy = source;
    auto moved_source = std::move(copy);
    CHECK(!copy.stop_possible());
    CHECK(moved_source.get_token() == survivor);
    CHECK(moved_source.request_stop());
    CHECK(!source.request_stop() && source.stop_requested());
  }
  CHECK(survivor.stop_possible() && survivor.stop_requested());
  {
    awj::compat::stop_source source;
    survivor = source.get_token();
  }
  CHECK(!survivor.stop_possible() && !survivor.stop_requested());
  {
    awj::compat::stop_source source;
    auto token = source.get_token();
    std::atomic<int> first{};
    std::vector<awj::compat::scoped_worker> workers;
    for (int i = 0; i < 8; ++i)
      workers.emplace_back([&, local = source] () mutable {
        if (local.request_stop()) first.fetch_add(1);
      });
    workers.clear();
    CHECK(first.load() == 1 && token.stop_requested());
  }
  {
    awj::compat::stop_source source;
    const auto token = source.get_token();
    int publication = 0;
    std::atomic<bool> observed{};
    awj::compat::scoped_worker worker([&] {
      while (!token.stop_requested()) std::this_thread::yield();
      observed.store(publication == 42);
    });
    publication = 42;
    source.request_stop();
    worker.join();
    CHECK(observed.load());
  }
  static_assert(!std::is_copy_constructible_v<awj::compat::scoped_worker>);
  static_assert(std::is_nothrow_move_constructible_v<awj::compat::scoped_worker>);
  for (int failure_after : {0, 1, 3, 5}) {
    std::atomic<int> finished{};
    int launched = 0;
    std::vector<awj::compat::scoped_worker> workers;
    try {
      workers.reserve(5);
      for (int i = 0; i < 5; ++i) {
        if (i == failure_after) throw std::bad_alloc{};
        workers.emplace_back([&] { finished.fetch_add(1); });
        ++launched;
      }
    } catch (const std::bad_alloc&) {}
    workers.clear();
    CHECK(finished.load() == launched && launched == failure_after);
  }
  {
    std::atomic<int> finished{};
    {
      awj::compat::scoped_worker first([&] { finished.fetch_add(1); });
      awj::compat::scoped_worker second([&] { finished.fetch_add(1); });
      first = std::move(second);
      CHECK(!second.joinable());
    }
    CHECK(finished.load() == 2);
  }
  {
    std::array<int, 5> input{3, 1, 4, 1, 5};
    std::array<int, 5> output{};
    const auto copied = awj::compat::ranges::copy(input, output.begin());
    CHECK(copied.in == input.end() && copied.out == output.end() && output == input);
    const auto transformed = awj::compat::ranges::transform(input, output.begin(), [](int value) { return value * 2; });
    CHECK(transformed.in == input.end() && transformed.out == output.end() && output[0] == 6);
    const auto partial = awj::compat::ranges::copy_n(input.begin(), 3, output.begin());
    CHECK(partial.in == input.begin() + 3 && partial.out == output.begin() + 3 && output[2] == 4);
    const auto zero = awj::compat::ranges::copy_n(input.begin(), 0, output.begin());
    CHECK(zero.in == input.begin() && zero.out == output.begin());
#if AWJ_USE_PORTABLE_STDLIB
    // 私有适配额外防御负计数；不把这项扩展作为桌面标准库契约。
    const auto negative = awj::compat::ranges::copy_n(input.begin(), -1, output.begin());
    CHECK(negative.in == input.begin() && negative.out == output.begin());
#endif
    CHECK(awj::compat::ranges::all_of(input, [](int value) { return value > 0; }));
    CHECK(awj::compat::ranges::any_of(input, [](int value) { return value == 4; }));
    CHECK(awj::compat::ranges::none_of(input, [](int value) { return value < 0; }));
    CHECK(!awj::compat::ranges::equal(input, std::array<int, 2>{3, 1}));
    CHECK(awj::compat::ranges::fill(output, 7) == output.end());
    CHECK(awj::compat::ranges::replace_if(output, [](int value) { return value == 7; }, 9) == output.end());
    CHECK(output[0] == 9 && output[4] == 9);
    struct Record { int key; };
    std::array<Record, 3> records{{{3}, {1}, {2}}};
    CHECK(awj::compat::ranges::sort(records, {}, &Record::key) == records.end());
    CHECK(records[0].key == 1 && records[2].key == 3);
    CHECK(awj::compat::ranges::find(records, 2, &Record::key) == records.begin() + 1);
    CHECK(awj::compat::ranges::find_if(records, [](int value) { return value == 3; }, &Record::key) == records.begin() + 2);
    CHECK(awj::compat::ranges::max_element(records, {}, &Record::key) == records.begin() + 2);
    std::array<int, 0> empty{};
    CHECK(awj::compat::ranges::max_element(empty) == empty.end());
  }
  std::puts("expected、UTC 宽时间格式、共享取消生命周期和 worker join 契约通过。");
  return 0;
}
