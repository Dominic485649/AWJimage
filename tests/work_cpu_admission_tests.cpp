#include "../src/core/work_cpu_admission.h"

#include <algorithm>
#include <array>
#include <barrier>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <latch>
#include <limits>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
using Admission = awj::WorkCpuAdmission;
using Lease = Admission::Lease;

static_assert(!std::is_copy_constructible_v<Lease>);
static_assert(!std::is_copy_assignable_v<Lease>);
static_assert(std::is_nothrow_move_constructible_v<Lease>);
static_assert(std::is_nothrow_move_assignable_v<Lease>);
static_assert(std::is_nothrow_destructible_v<Lease>);

void check(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "%s\n", message);
    std::abort();  // Checks remain enabled in Release, including worker checks.
  }
}

void check_conservation(const Admission& admission) {
  const auto state = admission.snapshot();
  check(state.budget >= 1 && state.budget <= Admission::max_budget,
        "Invalid resolved CPU budget.");
  check(state.used >= state.active_leases && state.used <= state.budget &&
            state.active_leases >= 0,
        "CPU ledger violated 0 <= active leases <= used <= budget.");
}

void check_empty(const Admission& admission) {
  check_conservation(admission);
  const auto state = admission.snapshot();
  check(state.used == 0 && state.active_leases == 0 && state.waiting == 0,
        "CPU units or waiting requests leaked.");
}

Lease take(Admission& admission, int demand) {
  auto result = admission.acquire(demand);
  check(result.has_value(), "An uncancelled acquisition returned no lease.");
  return std::move(*result);
}

// A state handshake, not a timing assertion. The holder keeps the entire budget
// until this observes queue registration, so cancellation tests really wake a
// queued request rather than merely passing a pre-stopped token. The test runner
// should impose a process timeout for deadlock detection; no sleeps/deadlines
// determine the correctness of these tests.
void await_waiters(const Admission& admission, std::size_t count) {
  for (;;) {
    check_conservation(admission);
    const auto state = admission.snapshot();
    if (state.waiting == count) return;
    check(state.waiting < count, "Unexpected request entered the CPU queue.");
    std::this_thread::yield();
  }
}

struct Gate {
  std::latch acquired{1};
  std::latch release{1};
};

void budgets_and_empty_leases() {
  for (const int invalid : {-1, 0, 129, std::numeric_limits<int>::max()}) {
    bool rejected = false;
    try {
      Admission admission{invalid};
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    check(rejected, "Out-of-range CPU budget was silently accepted.");
  }
  Admission admission{128, std::numeric_limits<std::size_t>::max()};
  check(admission.snapshot().runnable_hint == 128, "Runnable hint was not bounded.");
  auto lease = take(admission, std::numeric_limits<int>::max());
  check(lease.threads() == 1 && admission.snapshot().used == 1,
        "Expected future work was charged as active CPU or not reserved fairly.");
  admission.set_runnable_hint(0);
  check(lease.try_resize(std::numeric_limits<int>::max()) == 128,
        "A zero hint suppressed an actual holder or tail growth.");
  check(lease.try_resize(std::numeric_limits<int>::min()) == 1,
        "Demand normalization produced an invalid grant.");
  lease.reset();
  lease.reset();
  check(lease.threads() == 0 && !lease && lease.try_resize(128) == 0,
        "An empty lease manufactured CPU units.");
  check_empty(admission);
}

void concurrent_conservation(int budget, int files) {
  Admission admission{budget, static_cast<std::size_t>(files)};
  std::vector<int> grants(static_cast<std::size_t>(files));
  std::barrier<> start{files + 1};
  std::barrier<> observed{files + 1};
  std::barrier<> next_stage{files + 1};
  std::vector<std::jthread> workers;
  workers.reserve(static_cast<std::size_t>(files));
  constexpr int resize_rounds = 24;
  for (int i = 0; i < files; ++i) {
    workers.emplace_back([&, i] {
      start.arrive_and_wait();
      auto lease = take(admission, budget);
      grants[static_cast<std::size_t>(i)] = lease.threads();
      observed.arrive_and_wait();
      next_stage.arrive_and_wait();
      for (int round = 0; round < resize_rounds; ++round) {
        // Everyone owns a lease: a blocking upgrade would deadlock at the next
        // barrier. Different demands repeatedly exercise both shrink and grow.
        const int demand = 1 + (round * 3 + i) % budget;
        const int granted = lease.try_resize(demand);
        check(granted >= 1 && granted <= demand, "Resize exceeded its demand.");
        grants[static_cast<std::size_t>(i)] = granted;
        check_conservation(admission);
        observed.arrive_and_wait();
        next_stage.arrive_and_wait();
      }
    });
  }
  start.arrive_and_wait();
  for (int phase = 0; phase <= resize_rounds; ++phase) {
    observed.arrive_and_wait();
    int sum = 0;
    for (const int grant : grants) sum += grant;
    const auto state = admission.snapshot();
    check_conservation(admission);
    check(state.active_leases == files && state.waiting == 0 && state.used == sum,
          "Concurrent grants do not equal the shared ledger.");
    if (phase == 0) {
      check(sum == budget, "Integer remainder was stranded at initial admission.");
      const int quotient = budget / files;
      const int remainder = budget % files;
      int larger = 0;
      for (const int grant : grants) {
        check(grant == quotient || grant == quotient + 1,
              "Initial hint allowed a first-file monopoly.");
        larger += grant == quotient + 1 ? 1 : 0;
      }
      check(larger == remainder, "Remainder was granted more than once or lost.");
    }
    next_stage.arrive_and_wait();
  }
  workers.clear();  // join before inspecting the final ledger
  check_empty(admission);
}

void demand_remainder_and_tail() {
  {
    Admission admission{7, 3};
    auto first = take(admission, 7);
    auto middle = take(admission, 7);
    auto last = take(admission, 7);
    check(first.threads() == 3 && middle.threads() == 2 && last.threads() == 2,
          "7/3 did not assign the remainder in admission order.");
    middle.reset();
    auto replacement = take(admission, 7);
    check(replacement.threads() == 2, "A reused slot stole an incumbent's quota.");
    first.reset();
    admission.set_runnable_hint(2);
    check(last.try_resize(7) == 4 && replacement.try_resize(7) == 3,
          "Stable admission order was lost when a slot was removed/reused.");
    replacement.reset();
    admission.set_runnable_hint(1);
    check(last.try_resize(7) == 7, "The tail could not borrow released units.");
    last.reset();
    check_empty(admission);
  }
  {
    Admission admission{16, 3};
    auto serial = take(admission, 1);
    auto left = take(admission, 16);
    auto right = take(admission, 16);
    check(serial.threads() == 1 && left.threads() == 8 && right.threads() == 7,
          "Known serial demand was not redistributed with integer remainder.");
    left.reset();
    admission.set_runnable_hint(2);  // Memory currently fits only two images.
    check(serial.try_resize(16) == 8 && right.try_resize(16) == 8,
          "Two memory-runnable files could not use the full budget.");
    right.reset();
    admission.set_runnable_hint(1);  // Only one image remains memory-runnable.
    check(serial.try_resize(16) == 16, "A sole memory-runnable image stayed capped.");
    admission.set_runnable_hint(4);
    check(serial.threads() == 16 && admission.snapshot().used == 16,
          "Changing the hint revoked quota from an executing codec.");
    check(serial.try_resize(16) == 4 && admission.snapshot().active_leases == 1,
          "Safe-boundary shrink charged phantom holders or ignored the hint.");
    admission.set_runnable_hint(0);
    check(serial.try_resize(16) == 16, "Real holders were omitted by a zero hint.");
  }
}

void fifo_waiters() {
  Admission admission{1, 1};
  auto holder = take(admission, 1);
  std::array<Gate, 3> gates{};
  std::array<std::jthread, 3> workers{};
  for (std::size_t i = 0; i < workers.size(); ++i) {
    workers[i] = std::jthread{[&, i] {
      auto lease = take(admission, 1);
      gates[i].acquired.count_down();
      gates[i].release.wait();
    }};
    await_waiters(admission, i + 1);
    check(admission.snapshot().active_leases == 1 && admission.snapshot().used == 1,
          "A CPU waiter was counted as active.");
  }
  holder.reset();
  for (std::size_t i = 0; i < workers.size(); ++i) {
    // Only this FIFO position may acquire; its held lease prevents later ones
    // from masking an out-of-order admission by finishing quickly.
    gates[i].acquired.wait();
    const auto state = admission.snapshot();
    check(state.active_leases == 1 && state.used == 1 && state.waiting == 2 - i,
          "FIFO acquisition lost a waiter or over-granted budget one.");
    gates[i].release.count_down();
    workers[i].join();
  }
  check_empty(admission);
}

void queued_floor_and_cooperative_shrink() {
  Admission admission{5, 1};
  auto holder = take(admission, 5);
  std::array<Gate, 2> gates{};
  std::array<int, 2> grants{};
  std::array<std::jthread, 2> workers{};
  for (std::size_t i = 0; i < workers.size(); ++i) {
    workers[i] = std::jthread{[&, i] {
      auto lease = take(admission, 5);
      grants[i] = lease.threads();
      gates[i].acquired.count_down();
      gates[i].release.wait();
    }};
    await_waiters(admission, i + 1);
  }
  admission.set_runnable_hint(0);  // Actual waiters still require a fair share.
  check(admission.snapshot().used == 5 && holder.threads() == 5,
        "Queued requests asynchronously revoked a running grant.");
  check(holder.try_resize(5) == 2, "A holder ignored queued files at a safe boundary.");
  for (auto& gate : gates) gate.acquired.wait();
  check(grants[0] == 2 && grants[1] == 1 && admission.snapshot().used == 5 &&
            admission.snapshot().active_leases == 3,
        "Queued-file floors or integer remainder were not honored.");
  check(holder.try_resize(5) == 2, "An incumbent exceeded its concurrent share.");
  for (auto& gate : gates) gate.release.count_down();
  for (auto& worker : workers) worker.join();
  admission.set_runnable_hint(1);
  check(holder.try_resize(5) == 5, "Released waiter quotas could not be borrowed.");
  holder.reset();
  check_empty(admission);
}

void cancellation_wakes_and_unlinks() {
  Admission admission{7, 1};
  std::stop_source already_stopped;
  already_stopped.request_stop();
  check(!admission.acquire(7, already_stopped.get_token()),
        "A pre-stopped acquire was admitted.");
  check_empty(admission);

  auto holder = take(admission, 7);
  std::array<std::stop_source, 3> stops{};
  std::array<bool, 3> acquired{};
  std::array<std::jthread, 3> workers{};
  for (std::size_t i = 0; i < workers.size(); ++i) {
    workers[i] = std::jthread{[&, i] {
      auto lease = admission.acquire(7, stops[i].get_token());
      acquired[i] = lease.has_value();
    }};
    await_waiters(admission, i + 1);
  }
  stops[1].request_stop();  // Remove an interior queue node.
  workers[1].join();
  check(!acquired[1] && admission.snapshot().waiting == 2,
        "Cancellation failed to unlink an interior waiter.");
  stops[0].request_stop();  // Remove the head without releasing any CPU.
  workers[0].join();
  check(!acquired[0] && admission.snapshot().waiting == 1 &&
            admission.snapshot().used == 7 && admission.snapshot().active_leases == 1,
        "Cancellation failed to wake/unlink the head or altered a running lease.");
  holder.reset();
  workers[2].join();
  check(acquired[2], "Cancelled predecessors stranded an uncancelled waiter.");
  check_empty(admission);

  auto running = take(admission, 7);
  std::stop_source stop;
  stop.request_stop();
  check(running.threads() == 7 && admission.snapshot().used == 7,
        "A stop request prematurely returned CPU still in use.");
  check(running.try_resize(7, stop.get_token()) == 0 && !running,
        "A cancelled safe-boundary resize did not release its lease.");
  check_empty(admission);
}

void cancellation_release_race(int budget) {
  for (int round = 0; round < 8; ++round) {
    Admission admission{budget, 1};
    auto holder = take(admission, budget);
    std::stop_source stop;
    std::barrier<> race{3};
    std::jthread waiter{[&] {
      // Either result is legal: cancellation may linearize before/after grant.
      // A successful temporary lease must still return every granted unit.
      auto lease = admission.acquire(budget, stop.get_token());
      if (lease) check_conservation(admission);
    }};
    await_waiters(admission, 1);
    std::jthread releaser{[&, lease = std::move(holder)]() mutable {
      race.arrive_and_wait();
      lease.reset();
    }};
    std::jthread canceller{[&] {
      race.arrive_and_wait();
      stop.request_stop();
    }};
    race.arrive_and_wait();
    releaser.join();
    canceller.join();
    waiter.join();
    check_empty(admission);
  }
}

struct SimulatedFailure {};

template <typename Entry>
void fail_to_start_worker(Entry entry) {
  // Deterministic constructor-failure model, NOT an actual OS thread failure.
  // A moved closure must unwind even when its body never starts.
  (void)entry;
  throw SimulatedFailure{};
}

void ownership_unwind_and_start_failure() {
  Admission admission{7, 2};
  auto first = take(admission, 7);
  auto second = take(admission, 7);
  Lease moved{std::move(first)};
  check(!first && first.threads() == 0 && moved.threads() == 4 &&
            admission.snapshot().used == 7,
        "Move construction duplicated or lost a lease.");
  second = std::move(moved);
  check(!moved && second.threads() == 4 && admission.snapshot().used == 4 &&
            admission.snapshot().active_leases == 1,
        "Move assignment did not release the overwritten lease.");
  auto* self = &second;
  second = std::move(*self);
  check(second.threads() == 4 && admission.snapshot().used == 4,
        "Self move returned a still-owned grant.");
  second.reset();
  check_empty(admission);

  try {
    auto left = take(admission, 7);
    auto right = take(admission, 7);
    throw SimulatedFailure{};
  } catch (const SimulatedFailure&) {}
  check_empty(admission);

  {
    Admission other{5, 1};
    auto left = take(admission, 7);
    auto right = take(other, 5);
    left = std::move(right);
    check_empty(admission);
    check(other.snapshot().used == 5 && left.threads() == 5 && !right,
          "Cross-ledger move returned units to the wrong owner.");
    left.reset();
    check_empty(other);
  }

  {
    Admission startup{16, 4};
    auto started = take(startup, 16);
    try {
      fail_to_start_worker([lease = take(startup, 16)] { (void)lease; });
    } catch (const SimulatedFailure&) {}
    check(startup.snapshot().used == 4 && startup.snapshot().active_leases == 1 &&
              startup.snapshot().waiting == 0,
          "A never-started worker closure leaked CPU units.");
    startup.set_runnable_hint(1);  // Coordinator reports actual started capacity.
    check(started.try_resize(16) == 16,
          "Partial launch failure left unreachable phantom reservations.");
    started.reset();
    check_empty(startup);
  }

  Lease survivor;
  {
    Admission temporary{7, 1};
    survivor = take(temporary, 7);
  }
  check(survivor.try_resize(3) == 3,
        "Lease did not own its ledger after facade destruction.");
  survivor.reset();
  check(!survivor, "Outliving lease did not release safely.");
}

void memory_growth_rollback_contract() {
  Admission admission{5, 1};
  auto lease = take(admission, 1);
  const int memory_safe = lease.threads();
  check(lease.try_resize(5) == 5, "CPU growth failed without contention.");
  // Integration pattern only: an independent, nonblocking memory reservation
  // rejects the larger workspace. No callback runs inside the CPU ledger.
  const bool workspace_admitted = false;
  if (!workspace_admitted)
    check(lease.try_resize(memory_safe) == 1,
          "Rejected memory growth did not return extra CPU units.");
  check(admission.snapshot().used == 1, "Rollback kept unbacked CPU work admitted.");
  lease.reset();  // Return CPU before retrying any blocking admission.
  check_empty(admission);
}
}  // namespace

int main() {
  try {
    budgets_and_empty_leases();
    for (const int budget : {1, 5, 7, 16}) {
      concurrent_conservation(budget, std::min(budget, budget == 16 ? 5 : 3));
      cancellation_release_race(budget);
    }
    demand_remainder_and_tail();
    fifo_waiters();
    queued_floor_and_cooperative_shrink();
    cancellation_wakes_and_unlinks();
    ownership_unwind_and_start_failure();
    memory_growth_rollback_contract();
  } catch (const std::exception& error) {
    std::fprintf(stderr, "CPU admission test threw: %s\n", error.what());
    return 1;
  }
  return 0;
}
