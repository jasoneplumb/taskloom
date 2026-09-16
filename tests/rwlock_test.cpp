#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <taskloom/rwlock.hpp>
#include <thread>
#include <vector>

TEST_CASE("exclusive and shared acquisition interact correctly") {
  taskloom::rwlock lock;

  SUBCASE("try_lock fails while a reader is inside") {
    REQUIRE(lock.try_lock_shared());
    CHECK_FALSE(lock.try_lock());
    lock.unlock_shared();
    CHECK(lock.try_lock());
    lock.unlock();
  }

  SUBCASE("try_lock_shared fails while a writer is inside") {
    REQUIRE(lock.try_lock());
    CHECK_FALSE(lock.try_lock_shared());
    lock.unlock();
    CHECK(lock.try_lock_shared());
    lock.unlock_shared();
  }

  SUBCASE("multiple readers coexist") {
    REQUIRE(lock.try_lock_shared());
    CHECK(lock.try_lock_shared());
    lock.unlock_shared();
    lock.unlock_shared();
  }
}

TEST_CASE("composes with std::shared_lock and std::unique_lock") {
  taskloom::rwlock lock;
  {
    std::shared_lock guard_a(lock);
    std::shared_lock guard_b(lock);
    CHECK(guard_a.owns_lock());
    CHECK(guard_b.owns_lock());
  }
  {
    std::unique_lock guard(lock);
    CHECK(guard.owns_lock());
  }
}

TEST_CASE("writer increments stay exact under reader pressure") {
  constexpr int kWriters = 2;
  constexpr int kReaders = 4;
  constexpr int kIncrements = 50000;

  taskloom::rwlock lock;
  std::uint64_t counter = 0;  // Protected by lock; deliberately not atomic.
  std::atomic<bool> stop{false};
  std::atomic<bool> monotonic{true};

  std::vector<std::thread> threads;
  threads.reserve(kWriters + kReaders);

  for (int w = 0; w < kWriters; ++w) {
    threads.emplace_back([&] {
      for (int i = 0; i < kIncrements; ++i) {
        std::unique_lock guard(lock);
        ++counter;
      }
    });
  }
  for (int r = 0; r < kReaders; ++r) {
    threads.emplace_back([&] {
      std::uint64_t last = 0;
      while (!stop.load(std::memory_order_relaxed)) {
        std::shared_lock guard(lock);
        // Reads must be monotonic: a torn or stale view would go backwards.
        if (counter < last) {
          monotonic.store(false, std::memory_order_relaxed);
        }
        last = counter;
      }
    });
  }

  for (int w = 0; w < kWriters; ++w) {
    threads[w].join();
  }
  stop.store(true, std::memory_order_relaxed);
  for (int t = kWriters; t < kWriters + kReaders; ++t) {
    threads[t].join();
  }

  CHECK(monotonic.load());
  std::unique_lock guard(lock);
  CHECK(counter == static_cast<std::uint64_t>(kWriters) * kIncrements);
}
