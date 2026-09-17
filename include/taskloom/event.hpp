#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>
#include <mutex>
#include <vector>

namespace taskloom {

// Completion-event node for dependency graphs: a node fires on_ready() once
// every prerequisite it registered against has completed. Edges may be added
// concurrently with completions; a registration that arrives after the
// prerequisite completed is notified immediately instead of being lost.
//
// Lifecycle contract: nodes outlive every edge that references them, and
// reset() may only run while no completions or registrations are in flight.
// on_ready() fires at most once per cycle (between resets): adding an edge
// to a node that may already have fired is a contract violation: the latch
// suppresses a second fire and debug builds assert. on_ready() must not
// throw; an exception would strand the remaining dependents. If a
// registration fails with an allocation exception, the dependent may fire
// during unwinding (as though the failed edge never existed), so callers
// that catch and retry must expect it to have fired already.
class event {
 public:
  event() = default;
  event(const event&) = delete;
  event& operator=(const event&) = delete;
  virtual ~event() = default;

  // Registers `dependent` to be notified when this node completes.
  void precedes(event& dependent) {
    dependent.deps_remaining_.fetch_add(1, std::memory_order_relaxed);
    bool already_complete = false;
    {
      std::unique_lock guard(lock_);
      if (completed_.load(std::memory_order_relaxed)) {
        already_complete = true;
      } else {
        try {
          dependents_.push_back(&dependent);
        } catch (...) {
          guard.unlock();
          // The edge was never made; hand the reservation back. If every
          // other prerequisite already landed, this fires the dependent as
          // though the failed edge never existed.
          dependent.notify();
          throw;
        }
      }
    }
    if (already_complete) {
      dependent.notify();
    }
  }

  // This node waits for `prerequisite` before becoming ready.
  void depends_on(event& prerequisite) { prerequisite.precedes(*this); }

  // Marks this node complete and notifies every registered dependent.
  // Runs the notifications outside the lock so an on_ready() that builds
  // more graph cannot deadlock against this node.
  void complete() noexcept {
    std::vector<event*> to_notify;
    {
      std::unique_lock guard(lock_);
      assert(!completed_.load(std::memory_order_relaxed) &&
             "complete() ran twice without reset()");
      completed_.store(true, std::memory_order_release);
      to_notify.swap(dependents_);
    }
    for (event* dependent : to_notify) {
      dependent->notify();
    }
  }

  bool is_complete() const noexcept {
    return completed_.load(std::memory_order_acquire);
  }

  std::uint32_t pending_prerequisites() const noexcept {
    return deps_remaining_.load(std::memory_order_acquire);
  }

  // Returns the node to its initial state. Caller guarantees quiescence.
  void reset() {
    std::unique_lock guard(lock_);
    completed_.store(false, std::memory_order_release);
    fired_.store(false, std::memory_order_relaxed);
    dependents_.clear();
    deps_remaining_.store(0, std::memory_order_relaxed);
  }

 protected:
  // Called at most once per cycle, on the thread that delivered the final
  // notification. Must not throw: an exception would strand the remaining
  // dependents of the completing prerequisite.
  virtual void on_ready() noexcept = 0;

 private:
  // One prerequisite finished; fires on_ready() when the last one does.
  // Private: every call must balance a prior registration, which only
  // precedes()/complete() can guarantee.
  void notify() noexcept {
    const std::uint32_t previous =
        deps_remaining_.fetch_sub(1, std::memory_order_acq_rel);
    assert(previous != 0 && "notify() without a registered prerequisite");
    if (previous == 1) {
      // Relaxed suffices: RMWs on fired_ are totally ordered by its
      // modification order, so exactly one exchange returns false, and the
      // happens-before for on_ready's data comes from deps_remaining_.
      if (!fired_.exchange(true, std::memory_order_relaxed)) {
        on_ready();
      } else {
        assert(false && "edge added to a node after it fired; reset() first");
      }
    }
  }

  std::atomic<std::uint32_t> deps_remaining_{0};
  std::atomic<bool> completed_{false};
  std::atomic<bool> fired_{false};
  std::mutex lock_;
  std::vector<event*> dependents_;
};

}  // namespace taskloom
