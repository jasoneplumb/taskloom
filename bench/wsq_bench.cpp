// Work-stealing deque throughput, measured against a mutex-guarded
// std::deque baseline on the identical workload. Fixed-work benchmarks:
// wall time is measured until every produced item has been consumed
// exactly once, so a result is only reported when conservation holds.
//
// Usage: wsq_bench [items] [max_thieves]

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <optional>
#include <taskloom/stats.hpp>
#include <taskloom/wsq.hpp>
#include <thread>
#include <vector>

namespace {

using bench_clock = std::chrono::steady_clock;

double seconds_since(bench_clock::time_point start) {
  return std::chrono::duration<double>(bench_clock::now() - start).count();
}

void report(const char* name, std::int64_t items, double seconds,
            const char* extra) {
  std::printf("%-28s %9.2f Mitems/s  (%lld items, %.3f s)%s%s\n", name,
              static_cast<double>(items) / seconds / 1e6,
              static_cast<long long>(items), seconds, *extra ? "  " : "",
              extra);
}

// Baseline structure: the straightforward alternative to a lock-free deque.
// Owner pushes and pops at the back, thieves take from the front, every
// operation under one mutex. Same API shape as taskloom::wsq.
class locked_deque {
 public:
  void push(std::intptr_t value) {
    std::lock_guard guard(mutex_);
    items_.push_back(value);
  }

  std::optional<std::intptr_t> pop() {
    std::lock_guard guard(mutex_);
    if (items_.empty()) {
      return std::nullopt;
    }
    const std::intptr_t value = items_.back();
    items_.pop_back();
    return value;
  }

  std::optional<std::intptr_t> steal() {
    std::lock_guard guard(mutex_);
    if (items_.empty()) {
      return std::nullopt;
    }
    const std::intptr_t value = items_.front();
    items_.pop_front();
    return value;
  }

 private:
  std::mutex mutex_;
  std::deque<std::intptr_t> items_;
};

// Owner alone, no contention: push/pop in batches.
template <typename Queue>
void bench_push_pop(const char* label, Queue& queue, std::int64_t items) {
  constexpr std::int64_t kBatch = 256;
  std::int64_t processed = 0;

  const auto start = bench_clock::now();
  while (processed < items) {
    for (std::int64_t i = 0; i < kBatch; ++i) {
      queue.push(i);
    }
    for (std::int64_t i = 0; i < kBatch; ++i) {
      if (!queue.pop()) {
        std::fprintf(stderr, "%s push/pop: conservation violated\n", label);
        std::exit(1);
      }
    }
    processed += kBatch;
  }
  char name[48];
  std::snprintf(name, sizeof(name), "%s push/pop", label);
  report(name, processed, seconds_since(start), "");
}

// One producing owner, N stealing thieves; owner pops every 64th item.
template <typename Queue>
void bench_steal(const char* label, Queue& queue, std::int64_t items,
                 int thieves) {
  std::atomic<bool> done{false};
  std::atomic<std::int64_t> consumed{0};

  // Per-thread consumption counts aggregate through the stats module.
  std::vector<taskloom::double_stats> per_thief(
      static_cast<std::size_t>(thieves));

  std::vector<std::thread> workers;
  workers.reserve(static_cast<std::size_t>(thieves));

  for (int n = 0; n < thieves; ++n) {
    workers.emplace_back([&, n] {
      std::int64_t mine = 0;
      while (!done.load(std::memory_order_relaxed)) {
        if (queue.steal()) {
          ++mine;
          consumed.fetch_add(1, std::memory_order_relaxed);
        }
      }
      while (queue.steal()) {
        ++mine;
        consumed.fetch_add(1, std::memory_order_relaxed);
      }
      per_thief[static_cast<std::size_t>(n)].add(static_cast<double>(mine));
    });
  }

  // Clock starts at first production: thieves spawned above idle on an
  // empty queue until now. It stops after the joins, because the metric is
  // fixed work, time until every item is consumed, and the final drain is
  // consumption, not shutdown.
  const auto start = bench_clock::now();
  for (std::int64_t i = 0; i < items; ++i) {
    queue.push(i);
    if ((i & 63) == 0) {
      if (queue.pop()) {
        consumed.fetch_add(1, std::memory_order_relaxed);
      }
    }
  }
  while (queue.pop()) {
    consumed.fetch_add(1, std::memory_order_relaxed);
  }
  done.store(true, std::memory_order_relaxed);
  for (auto& w : workers) {
    w.join();
  }
  const double elapsed = seconds_since(start);

  if (consumed.load() != items) {
    std::fprintf(stderr, "%s steal x%d: conservation violated (%lld/%lld)\n",
                 label, thieves, static_cast<long long>(consumed.load()),
                 static_cast<long long>(items));
    std::exit(1);
  }

  taskloom::double_stats stolen;
  for (const auto& s : per_thief) {
    stolen.merge(s);
  }
  char extra[96];
  std::snprintf(extra, sizeof(extra), "stolen %.1f%%, per-thief cv %.2f",
                100.0 * stolen.total() / static_cast<double>(items),
                thieves > 1 ? stolen.coefficient_of_variation() : 0.0);
  char name[48];
  std::snprintf(name, sizeof(name), "%s steal x%d", label, thieves);
  report(name, items, elapsed, extra);
}

}  // namespace

int main(int argc, char** argv) {
  const std::int64_t items =
      argc > 1 ? std::strtoll(argv[1], nullptr, 10) : 2000000;
  const int max_thieves =
      argc > 2 ? std::atoi(argv[2])
               : static_cast<int>(std::min(
                     4u, std::max(2u, std::thread::hardware_concurrency()) -
                             1u));

  if (items <= 0 || max_thieves < 1) {
    std::fprintf(stderr, "usage: wsq_bench [items > 0] [max_thieves >= 1]\n");
    return 1;
  }
  std::printf("wsq_bench: %lld items, up to %d thieves\n",
              static_cast<long long>(items), max_thieves);

  {
    // Depth never exceeds one 256-item batch here; 1024 slots avoid growth.
    taskloom::wsq<std::intptr_t> queue(1024);
    bench_push_pop("wsq", queue, items);
  }
  {
    locked_deque queue;
    bench_push_pop("locked_deque", queue, items);
  }
  for (int thieves = 1; thieves <= max_thieves; thieves *= 2) {
    {
      // Pre-sized to worst-case depth (capped at 2^21 slots) so ring growth
      // and its allocator latency stay out of the measured window for runs
      // up to the cap; larger runs will grow mid-benchmark.
      taskloom::wsq<std::intptr_t> queue(
          static_cast<std::size_t>(std::min<std::int64_t>(items, 1 << 21)));
      bench_steal("wsq", queue, items, thieves);
    }
    {
      locked_deque queue;
      bench_steal("locked_deque", queue, items, thieves);
    }
  }
  return 0;
}
