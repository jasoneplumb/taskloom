// Reader-writer lock acquisition throughput under a read-heavy workload,
// with std::shared_mutex on the identical workload as the reference.
// Fixed work: each reader performs a set number of shared acquisitions
// while one writer updates continuously; wall time covers all readers.
//
// Usage: rwlock_bench [acquisitions_per_reader] [max_readers]

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <shared_mutex>
#include <taskloom/rwlock.hpp>
#include <thread>
#include <vector>

namespace {

using bench_clock = std::chrono::steady_clock;

template <typename Lock>
double run_read_heavy(int readers, std::int64_t acquisitions) {
  Lock lock;
  std::uint64_t shared_value = 0;  // Guarded by lock.
  std::atomic<bool> stop_writer{false};
  std::atomic<std::uint64_t> sink{0};

  std::thread writer([&] {
    while (!stop_writer.load(std::memory_order_relaxed)) {
      {
        std::unique_lock guard(lock);
        ++shared_value;
      }
      for (int i = 0; i < 64; ++i) {
        // Keep the writer occasional rather than saturating.
        std::this_thread::yield();
      }
    }
  });

  // Start gate: readers spawn, park on the flag, and begin together once
  // the clock is running, keeping thread-creation cost out of the window.
  std::atomic<bool> go{false};
  std::vector<std::thread> pool;
  pool.reserve(static_cast<std::size_t>(readers));
  for (int r = 0; r < readers; ++r) {
    pool.emplace_back([&] {
      while (!go.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      std::uint64_t local = 0;
      for (std::int64_t i = 0; i < acquisitions; ++i) {
        std::shared_lock guard(lock);
        local += shared_value;
      }
      sink.fetch_add(local, std::memory_order_relaxed);
    });
  }
  const auto start = bench_clock::now();
  go.store(true, std::memory_order_release);
  for (auto& t : pool) {
    t.join();
  }
  const double elapsed =
      std::chrono::duration<double>(bench_clock::now() - start).count();
  stop_writer.store(true, std::memory_order_relaxed);
  writer.join();

  if (sink.load() == std::uint64_t{0} - 1) {
    std::printf("unreachable\n");  // Defeats over-eager dead-code removal.
  }
  return elapsed;
}

void compare(int readers, std::int64_t acquisitions) {
  const double ours = run_read_heavy<taskloom::rwlock>(readers, acquisitions);
  const double stds = run_read_heavy<std::shared_mutex>(readers, acquisitions);
  const double total = static_cast<double>(acquisitions) * readers;
  std::printf(
      "readers x%-2d  taskloom::rwlock %8.2f Macq/s   std::shared_mutex "
      "%8.2f Macq/s\n",
      readers, total / ours / 1e6, total / stds / 1e6);
}

}  // namespace

int main(int argc, char** argv) {
  const std::int64_t acquisitions =
      argc > 1 ? std::strtoll(argv[1], nullptr, 10) : 1000000;
  const int max_readers =
      argc > 2 ? std::atoi(argv[2])
               : static_cast<int>(std::min(
                     4u, std::max(2u, std::thread::hardware_concurrency()) -
                             1u));

  if (acquisitions <= 0 || max_readers < 1) {
    std::fprintf(stderr,
                 "usage: rwlock_bench [acquisitions > 0] [max_readers >= 1]\n");
    return 1;
  }
  std::printf("rwlock_bench: %lld shared acquisitions per reader, up to %d "
              "readers, one occasional writer\n",
              static_cast<long long>(acquisitions), max_readers);
  for (int readers = 1; readers <= max_readers; readers *= 2) {
    compare(readers, acquisitions);
  }
  return 0;
}
