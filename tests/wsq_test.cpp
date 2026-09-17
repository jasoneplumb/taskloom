#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <taskloom/wsq.hpp>
#include <thread>
#include <vector>

TEST_CASE("owner pops newest, thief steals oldest") {
  taskloom::wsq<std::intptr_t> queue(8);
  queue.push(1);
  queue.push(2);
  queue.push(3);
  CHECK(queue.size() == 3);

  auto stolen = queue.steal();
  REQUIRE(stolen.has_value());
  CHECK(*stolen == 1);

  auto popped = queue.pop();
  REQUIRE(popped.has_value());
  CHECK(*popped == 3);

  popped = queue.pop();
  REQUIRE(popped.has_value());
  CHECK(*popped == 2);

  CHECK_FALSE(queue.pop().has_value());
  CHECK_FALSE(queue.steal().has_value());
  CHECK(queue.empty());
}

TEST_CASE("growth preserves every element") {
  taskloom::wsq<std::intptr_t> queue(2);
  constexpr std::intptr_t kCount = 1000;
  for (std::intptr_t i = 0; i < kCount; ++i) {
    queue.push(i);
  }
  CHECK(queue.capacity() >= static_cast<std::size_t>(kCount));
  CHECK(queue.size() == static_cast<std::size_t>(kCount));

  // Pop everything: LIFO means kCount-1 down to 0, each exactly once.
  for (std::intptr_t expected = kCount - 1; expected >= 0; --expected) {
    auto value = queue.pop();
    REQUIRE(value.has_value());
    CHECK(*value == expected);
  }
  CHECK(queue.empty());
}

TEST_CASE("single element race between owner and thief resolves exactly once") {
  constexpr int kRounds = 3000;
  for (int round = 0; round < kRounds; ++round) {
    taskloom::wsq<std::intptr_t> queue(4);
    queue.push(42);

    std::optional<std::intptr_t> stolen;
    std::thread thief([&] { stolen = queue.steal(); });
    const std::optional<std::intptr_t> popped = queue.pop();
    thief.join();

    const int winners = (stolen.has_value() ? 1 : 0) +
                        (popped.has_value() ? 1 : 0);
    CHECK(winners == 1);
    if (stolen) {
      CHECK(*stolen == 42);
    }
    if (popped) {
      CHECK(*popped == 42);
    }
  }
}

TEST_CASE("conservation under concurrent stealing with growth") {
  constexpr std::intptr_t kItems = 100000;
  constexpr int kThieves = 3;

  taskloom::wsq<std::intptr_t> queue(2);  // Tiny start forces many grows.
  std::vector<std::atomic<std::uint8_t>> seen(kItems);
  std::atomic<bool> done{false};
  std::atomic<std::intptr_t> consumed{0};

  auto consume = [&](std::intptr_t item) {
    seen[static_cast<std::size_t>(item)].fetch_add(1,
                                                   std::memory_order_relaxed);
    consumed.fetch_add(1, std::memory_order_relaxed);
  };

  std::vector<std::thread> thieves;
  thieves.reserve(kThieves);
  for (int i = 0; i < kThieves; ++i) {
    thieves.emplace_back([&] {
      while (!done.load(std::memory_order_relaxed)) {
        if (auto item = queue.steal()) {
          consume(*item);
        }
      }
      // Final drain so nothing is stranded between the flag and the join.
      while (auto item = queue.steal()) {
        consume(*item);
      }
    });
  }

  // Owner: push everything, popping a batch every so often.
  for (std::intptr_t i = 0; i < kItems; ++i) {
    queue.push(i);
    if ((i & 63) == 0) {
      if (auto item = queue.pop()) {
        consume(*item);
      }
    }
  }
  while (auto item = queue.pop()) {
    consume(*item);
  }
  done.store(true, std::memory_order_relaxed);
  for (auto& t : thieves) {
    t.join();
  }

  CHECK(consumed.load() == kItems);
  std::intptr_t mismatches = 0;
  for (std::intptr_t i = 0; i < kItems; ++i) {
    if (seen[static_cast<std::size_t>(i)].load() != 1) {
      ++mismatches;
    }
  }
  CHECK(mismatches == 0);
}
