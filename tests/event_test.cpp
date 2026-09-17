#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <atomic>
#include <cstdint>
#include <taskloom/event.hpp>
#include <thread>
#include <vector>

namespace {

class counting_event : public taskloom::event {
 public:
  std::atomic<std::uint32_t> fired{0};

 protected:
  void on_ready() noexcept override {
    fired.fetch_add(1, std::memory_order_relaxed);
  }
};

}  // namespace

TEST_CASE("a node fires once after all prerequisites complete") {
  counting_event a;
  counting_event c;
  counting_event b;
  b.depends_on(a);
  b.depends_on(c);
  CHECK(b.pending_prerequisites() == 2);

  a.complete();
  CHECK(b.fired.load() == 0);
  c.complete();
  CHECK(b.fired.load() == 1);
  CHECK(a.is_complete());
  CHECK(c.is_complete());
}

TEST_CASE("registration after completion notifies immediately") {
  counting_event a;
  counting_event b;
  a.complete();
  b.depends_on(a);
  CHECK(b.fired.load() == 1);
}

TEST_CASE("both edge directions are equivalent") {
  counting_event a;
  counting_event b;
  a.precedes(b);
  a.complete();
  CHECK(b.fired.load() == 1);
}

TEST_CASE("many dependents grow past any fixed capacity") {
  counting_event hub;
  std::vector<counting_event> dependents(64);
  for (auto& d : dependents) {
    d.depends_on(hub);
  }
  hub.complete();
  for (auto& d : dependents) {
    CHECK(d.fired.load() == 1);
  }
}

TEST_CASE("reset returns a node to service") {
  counting_event a;
  counting_event b;
  b.depends_on(a);
  a.complete();
  CHECK(b.fired.load() == 1);

  a.reset();
  b.reset();
  b.fired.store(0);
  CHECK_FALSE(a.is_complete());
  b.depends_on(a);
  a.complete();
  CHECK(b.fired.load() == 1);
}

TEST_CASE("concurrent completion and registration never lose a notification") {
  constexpr int kRounds = 2000;
  for (int round = 0; round < kRounds; ++round) {
    counting_event prerequisite;
    counting_event dependent;

    std::thread completer([&] { prerequisite.complete(); });
    std::thread registrar([&] { dependent.depends_on(prerequisite); });
    completer.join();
    registrar.join();

    // Whichever side won the race, exactly one notification must land.
    CHECK(dependent.fired.load() == 1);
    CHECK(dependent.pending_prerequisites() == 0);
  }
}

TEST_CASE("fan-in from many completing threads fires the sink exactly once") {
  constexpr int kProducers = 8;
  counting_event sink;
  std::vector<counting_event> producers(kProducers);
  for (auto& p : producers) {
    sink.depends_on(p);
  }

  std::vector<std::thread> threads;
  threads.reserve(kProducers);
  for (auto& p : producers) {
    threads.emplace_back([&p] { p.complete(); });
  }
  for (auto& t : threads) {
    t.join();
  }
  CHECK(sink.fired.load() == 1);
}
