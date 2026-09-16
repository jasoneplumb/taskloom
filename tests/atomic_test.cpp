#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <atomic>
#include <cstdint>
#include <taskloom/atomic.hpp>
#include <thread>
#include <vector>

TEST_CASE("padded elements occupy distinct false-sharing ranges") {
  taskloom::padded<std::uint32_t> pair[2];
  const auto a = reinterpret_cast<std::uintptr_t>(&pair[0].value);
  const auto b = reinterpret_cast<std::uintptr_t>(&pair[1].value);
  REQUIRE(b > a);
  CHECK(b - a >= taskloom::false_sharing_bytes);
}

TEST_CASE("padded atomics are independently writable from many threads") {
  constexpr int kThreads = 4;
  constexpr int kIncrements = 100000;
  std::vector<taskloom::padded<std::atomic<std::uint64_t>>> slots(kThreads);

  std::vector<std::thread> workers;
  workers.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    workers.emplace_back([&slots, t] {
      for (int i = 0; i < kIncrements; ++i) {
        slots[t].value.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  for (auto& w : workers) {
    w.join();
  }

  for (int t = 0; t < kThreads; ++t) {
    CHECK(slots[t].value.load(std::memory_order_relaxed) == kIncrements);
  }
}

TEST_CASE("heap allocation honors the over-aligned padded type") {
  auto* slot = new taskloom::padded<std::atomic<std::uint64_t>>;
  const auto address = reinterpret_cast<std::uintptr_t>(slot);
  CHECK(address % taskloom::false_sharing_bytes == 0);
  delete slot;
}
