#pragma once

#include <atomic>
#include <cstdint>

#include "atomic.hpp"
#include "hw.hpp"

namespace taskloom {

// Reader-writer spinlock: any number of concurrent readers or one writer.
// Writer-preferring: a reader that arrives while a writer holds or claims
// the lock backs out and retries, so writers are not starved by a steady
// reader stream. The two counters live on separate false-sharing ranges so
// reader traffic does not invalidate the writer flag's cache line.
//
// Meets the SharedLockable / Lockable requirements, so it composes with
// std::shared_lock and std::unique_lock. Spins with cpu_pause(); intended
// for short critical sections, not for waits long enough to deserve a
// blocking mutex.
class rwlock {
 public:
  rwlock() = default;
  rwlock(const rwlock&) = delete;
  rwlock& operator=(const rwlock&) = delete;

  // Ordering note: acquisition is an announce-then-check-the-other-side
  // protocol across two variables (the store-buffering / Dekker pattern).
  // Release/acquire alone still allows both sides to miss each other's
  // announcement, so the announce and check operations are seq_cst; the
  // single total order then guarantees at least one side observes the
  // other. Unlocks and back-outs only publish data and stay release.
  [[nodiscard]] bool try_lock_shared() noexcept {
    readers_.value.fetch_add(1, std::memory_order_seq_cst);
    if (writer_.value.load(std::memory_order_seq_cst) != 0) {
      // A writer holds or is claiming the lock: back out and report failure.
      readers_.value.fetch_sub(1, std::memory_order_release);
      return false;
    }
    return true;
  }

  void lock_shared() noexcept {
    while (!try_lock_shared()) {
      while (writer_.value.load(std::memory_order_relaxed) != 0) {
        cpu_pause();
      }
    }
  }

  void unlock_shared() noexcept {
    readers_.value.fetch_sub(1, std::memory_order_release);
  }

  [[nodiscard]] bool try_lock() noexcept {
    std::uint32_t expected = 0;
    if (!writer_.value.compare_exchange_strong(expected, 1,
                                               std::memory_order_seq_cst,
                                               std::memory_order_relaxed)) {
      return false;
    }
    if (readers_.value.load(std::memory_order_seq_cst) != 0) {
      // Readers are inside (or backing out); a try call must not wait.
      writer_.value.store(0, std::memory_order_release);
      return false;
    }
    return true;
  }

  void lock() noexcept {
    for (;;) {
      std::uint32_t expected = 0;
      if (writer_.value.compare_exchange_weak(expected, 1,
                                              std::memory_order_seq_cst,
                                              std::memory_order_relaxed)) {
        break;
      }
      // Test-and-test-and-set: spin on a plain load until the flag looks
      // free, so contended waiters do not fight over line ownership.
      while (writer_.value.load(std::memory_order_relaxed) != 0) {
        cpu_pause();
      }
    }
    // The flag is claimed, which turns arriving readers away; now drain the
    // ones already inside.
    while (readers_.value.load(std::memory_order_seq_cst) != 0) {
      cpu_pause();
    }
  }

  void unlock() noexcept {
    writer_.value.store(0, std::memory_order_release);
  }

 private:
  padded<std::atomic<std::uint32_t>> readers_{};
  padded<std::atomic<std::uint32_t>> writer_{};
};

}  // namespace taskloom
