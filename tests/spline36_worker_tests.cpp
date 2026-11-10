#include "../src/core/spline36_resample.h"
#include "spline36_legacy_reference.h"
#include <cstdio>
#include <mutex>
#include <thread>

namespace {
using namespace awj::spline36_detail;
constexpr std::size_t width = 512, height = 256, tw = 257, th = 129;
using Result = awj::compat::expected<void, std::string>;
#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "Spline36 worker line %d: %s\n", __LINE__, #value); return false; } } while (false)

std::array<float, 4> sample(std::size_t x, std::size_t y) {
  return {static_cast<float>((x * 17 + y * 31) % 1024) / 1023,
      static_cast<float>((x * 41 + y * 11) % 65536) / 65535,
      static_cast<float>((x + y * 19) % 256) / 255,
      static_cast<float>((x * 3 + y * 7) % 256) / 255};
}

struct Faults {
  int fail_launch{-1};
  bool launch_bad_alloc{};
  bool launch_unknown{};
  int fail_allocation{-1};
  bool only_scalar{};
  bool deny_allocation{};
  int allocations{}, launches{};
  std::atomic<int> running{}, finished{};
};

struct FaultOperations : WorkerOperations {
  Faults* faults{};
  explicit FaultOperations(Faults& state) : faults(&state) {}
  void allocate(WorkerScratch& scratch, std::size_t w, std::size_t target, const ScratchPlan& plan) const {
    const auto call = faults->allocations++;
    if (faults->deny_allocation || (faults->only_scalar && (plan.cache_source || plan.workers != 1))) throw std::bad_alloc{};
    WorkerOperations::allocate(scratch, w, target, plan);
    // 在真实的小额分配后抛出，覆盖部分 scratch 已构造时的清理。
    if (call == faults->fail_allocation) throw std::bad_alloc{};
  }
  template <class Work>
  awj::compat::scoped_worker launch(Work&& work) const {
    if (faults->launches++ == faults->fail_launch) {
      if (faults->launch_unknown) throw 7;
      if (faults->launch_bad_alloc) throw std::bad_alloc{};
      throw std::system_error{std::make_error_code(std::errc::resource_unavailable_try_again)};
    }
    return awj::compat::scoped_worker([state = faults, task = std::forward<Work>(work)]() mutable noexcept {
      state->running.fetch_add(1);
      task();
      state->running.fetch_sub(1);
      state->finished.fetch_add(1);
    });
  }
};

bool planning() {
  const auto full = make_plan(width, height, tw, th, 8);
  CHECK(full.workers == 8 && full.cache_source && full.cache_rows >= 12);
  CHECK(scratch_bytes(width, height, tw, th, 8) == full.bytes);
  const auto quarter = make_plan(width, height, width / 4, height / 4, 8);
  CHECK(quarter.cache_rows > 12);
  const auto fixed = full.coefficients + shared_control_bytes;
  const auto scalar_bytes = fixed + worker_bytes(width, tw, 1, false);
  const auto scalar = make_plan(width, height, tw, th, 8, scalar_bytes);
  CHECK(scalar.workers == 1 && !scalar.cache_source && scalar.cache_rows == 1 && scalar.bytes == scalar_bytes);
  CHECK(make_plan(width, height, tw, th, 8, scalar_bytes - 1).workers == 0);
  CHECK(make_plan(width, height, tw, th, 8, 0).workers == 0);
  const auto partial_bytes = fixed + worker_bytes(width, tw, 2, true);
  const auto partial = make_plan(width, height, tw, th, 8, partial_bytes);
  CHECK(partial.workers == 1 && partial.cache_source && partial.cache_rows == 2);
  const auto two = make_plan(width, height, tw, th, 8, fixed + worker_bytes(width, tw, full.cache_rows, true) * 2);
  CHECK(two.workers == 2 && two.cache_source);
  CHECK(make_plan(7, 5, 11, 8, 8).workers == 1);
  CHECK(make_plan(1, height, 1, 1, 8).workers == 1);
  CHECK(make_plan(width, height, width, 2, 1).cache_rows == 1);
  CHECK(make_plan(width, height, width, 2, 8).cache_rows == 1);
  CHECK(make_plan(0, height, tw, th, 8).workers == 0);
  CHECK(make_plan(SIZE_MAX, height, tw, th, 8).bytes == UINT64_MAX);
  for (const auto dimensions : {std::array<std::size_t, 2>{8, 8}, {14, 7}, {26, 7}, {256, 2}, {256, 64}}) {
    const auto axis = make_axis(dimensions[0], dimensions[1], {});
    CHECK(axis);
    std::size_t support{};
    for (const auto& entry : *axis) support = std::max(support, entry.weights.size());
    const auto planned = make_plan(width, dimensions[0], tw, dimensions[1], 8);
    const auto exact = make_plan(width, dimensions[0], tw, dimensions[1], 8, UINT64_MAX, support);
    CHECK(exact.bytes <= planned.bytes);
    CHECK(axis_storage_bytes(*axis) <= axis_bytes(dimensions[0], dimensions[1]));
  }
  for (const auto& [plan, target] : std::array{std::pair{full, tw}, std::pair{quarter, width / 4},
      std::pair{scalar, tw}, std::pair{partial, tw}, std::pair{two, tw}}) {
    WorkerScratch storage;
    WorkerOperations{}.allocate(storage, width, target, plan);
    const auto allocated = sizeof(WorkerScratch) +
        (storage.source.capacity() + storage.horizontal.capacity() + storage.accumulated.capacity()) * sizeof(Pixel) +
        storage.row_ids.capacity() * sizeof(std::size_t);
    CHECK(allocated <= worker_bytes(width, target, plan.cache_rows, plan.cache_source));
  }
  return true;
}

bool successful_run(const std::vector<Pixel>& reference, int budget, std::uint64_t available,
    Faults& faults, int expected_workers) {
  std::vector<Pixel> output(tw * th);
  std::vector<std::atomic<unsigned>> visits(tw * th);
  std::atomic<bool> duplicate{}, wrong_policy{};
  std::atomic<std::size_t> reads{};
  std::array<std::thread::id, 8> participants{};
  std::size_t participant_count{};
  std::mutex participants_mutex;
  const auto policy = *awj::memory_policy::resolve(true, false, 0);
  const awj::memory_policy::Scope policy_scope{policy};
  FilterDiagnostics diagnostic{77, UINT64_MAX};
  auto result = filter(width, height, tw, th,
      [&](std::size_t x, std::size_t y) {
        reads.fetch_add(1);
        if (awj::memory_policy::current() != policy) wrong_policy.store(true);
        return sample(x, y);
      }, [&](std::size_t x, std::size_t y, Pixel rgba) -> Result {
        const auto position = y * tw + x;
        if (visits[position].fetch_add(1)) { duplicate.store(true); return {}; }
        output[position] = rgba;
        if (x == 0) {
          const std::lock_guard lock{participants_mutex};
          const auto id = std::this_thread::get_id();
          if (std::find(participants.begin(), participants.begin() + participant_count, id) == participants.begin() + participant_count)
            participants[participant_count++] = id;
        }
        return {};
      }, {}, {.worker_budget = budget, .scratch_limit_bytes = available}, &diagnostic, FaultOperations{faults});
  CHECK(result && output == reference && !duplicate.load() && !wrong_policy.load());
  CHECK(diagnostic.workers == expected_workers && static_cast<std::size_t>(diagnostic.workers) == participant_count);
  CHECK(std::find(participants.begin(), participants.begin() + participant_count, std::this_thread::get_id()) != participants.begin() + participant_count);
  CHECK(diagnostic.scratch_bytes <= scratch_bytes(width, height, tw, th, budget) && diagnostic.scratch_bytes <= available);
  CHECK(faults.running.load() == 0 && faults.finished.load() == diagnostic.workers - 1);
  for (const auto& visited : visits) CHECK(visited.load() == 1);
  if (expected_workers == 1 && !faults.only_scalar && available == UINT64_MAX)
    CHECK(reads.load() == width * height); // 每个源像素只解码/映射一次，不按重叠 tap 重复读取。
  return true;
}

bool successes_and_fallbacks() {
  std::vector<Pixel> reference(tw * th);
  CHECK(awj::test::legacy_spline36(width, height, tw, th, sample,
      [&](std::size_t x, std::size_t y, Pixel rgba) -> Result { reference[y * tw + x] = rgba; return {}; }));
  for (const int budget : {1, 2, 8}) {
    Faults state;
    CHECK(successful_run(reference, budget, UINT64_MAX, state, budget));
  }
  const auto full = make_plan(width, height, tw, th, 8);
  const auto fixed = full.coefficients + shared_control_bytes;
  for (const bool cache_source : {false, true}) {
    Faults state;
    CHECK(successful_run(reference, 8, fixed + worker_bytes(width, tw, 1, cache_source), state, 1));
  }
  for (const int first_failure : {0, 1, 3}) for (const bool allocation_failure : {false, true}) {
    Faults state{.fail_launch = first_failure, .launch_bad_alloc = allocation_failure};
    CHECK(successful_run(reference, 8, UINT64_MAX, state, first_failure + 1));
  }
  {
    Faults state{.fail_allocation = 2};
    CHECK(successful_run(reference, 8, UINT64_MAX, state, 4));
  }
  {
    Faults state{.only_scalar = true};
    CHECK(successful_run(reference, 8, UINT64_MAX, state, 1));
  }
  return true;
}

bool failures_and_cancellation() {
  for (int mode = 0; mode < 12; ++mode) {
    awj::compat::stop_source cancel;
    Faults faults;
    if (mode == 0) cancel.request_stop();
    if (mode == 7) faults.deny_allocation = true;
    if (mode == 8) { faults.fail_launch = 2; faults.launch_unknown = true; }
    std::atomic<std::size_t> reads{}, writes{};
    FilterDiagnostics diagnostic{8, 123};
    auto result = filter(width, height, tw, th,
        [&](std::size_t x, std::size_t y) {
          const auto call = reads.fetch_add(1);
          if (call == 512) {
            if (mode == 1) cancel.request_stop();
            if (mode == 2) throw std::runtime_error{"read failed"};
            if (mode == 3) throw std::bad_alloc{};
            if (mode == 4) throw 42;
          }
          auto value = sample(x, y);
          if (mode == 5 && call == 512) { value[0] = std::numeric_limits<float>::quiet_NaN(); value[3] = 0; }
          return value;
        }, [&](std::size_t, std::size_t, Pixel) -> Result {
          if (writes.fetch_add(1) == 0) {
            if (mode == 6) return awj::compat::unexpected{std::string(4096, 'w')};
            if (mode == 9) throw std::runtime_error{"write failed"};
            if (mode == 10) throw std::bad_alloc{};
            if (mode == 11) throw std::length_error{"write length"};
          }
          return {};
        }, cancel.get_token(), {.worker_budget = 8}, &diagnostic, FaultOperations{faults});
    CHECK(!result && diagnostic.workers == 0 && faults.running.load() == 0);
    CHECK(faults.finished.load() == (mode == 8 ? 2 : mode == 0 || mode == 7 ? 0 : faults.launches));
    if (mode == 0 || mode == 7) CHECK(reads.load() == 0 && writes.load() == 0);
    if (mode == 6) CHECK(result.error() == std::string(4096, 'w'));
  }
  return true;
}
} // namespace

int main() {
  const awj::memory_policy::Scope policy{*awj::memory_policy::resolve(false, false, 1024ull * 1024 * 1024)};
  return planning() && successes_and_fallbacks() && failures_and_cancellation() ? 0 : 1;
}
