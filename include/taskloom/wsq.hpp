#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <vector>

#include "atomic.hpp"

namespace taskloom {

// Work-stealing deque (Chase-Lev), with the release/acquire/fence placement
// of the weak-memory formulation. One owner thread pushes and pops at the
// bottom; any number of thieves steal from the top. Owner operations are
// wait-free except when growing; steal is lock-free and may fail spuriously
// when it loses a race, in which case the caller picks another victim or
// retries.
//
// Differences from the ancestral implementation, deliberately:
// - Indices are monotonically increasing int64_t, so the 32-bit rollover
//   that broke empty()/count() cannot occur over any realistic lifetime.
// - Growth retires the old ring into a list owned by the deque instead of
//   freeing it while concurrent thieves may still hold it; retired rings
//   are reclaimed in the destructor.
// - Stealing is pure lock-free; the reader-writer lock arbitration of the
//   original is gone.
//
// T must be trivially copyable and lock-free as an atomic (a pointer or a
// small handle): elements live in std::atomic<T> slots because a thief may
// read a slot concurrently with the owner republishing it.
template <typename T>
class wsq {
  static_assert(std::is_trivially_copyable_v<T>,
                "elements are copied through atomic slots");
  static_assert(std::atomic<T>::is_always_lock_free,
                "store pointers or small handles, not payloads");

 public:
  explicit wsq(std::size_t initial_capacity = 64)
      : buffer_{new ring(checked_capacity(initial_capacity))} {}

  wsq(const wsq&) = delete;
  wsq& operator=(const wsq&) = delete;

  ~wsq() { delete buffer_.value.load(std::memory_order_relaxed); }

  // Owner only. Wait-free unless the ring is full, in which case it grows.
  void push(T value) {
    const std::int64_t b = bottom_.value.load(std::memory_order_relaxed);
    const std::int64_t t = top_.value.load(std::memory_order_acquire);
    ring* buf = buffer_.value.load(std::memory_order_relaxed);
    if (b - t >= static_cast<std::int64_t>(buf->capacity())) {
      buf = grow(buf, t, b);
    }
    buf->put(b, value);
    std::atomic_thread_fence(std::memory_order_release);
    bottom_.value.store(b + 1, std::memory_order_relaxed);
  }

  // Owner only. Takes the newest element (LIFO); empty-handed only when the
  // deque is empty or a thief won the last element.
  std::optional<T> pop() {
    const std::int64_t b = bottom_.value.load(std::memory_order_relaxed) - 1;
    ring* buf = buffer_.value.load(std::memory_order_relaxed);
    bottom_.value.store(b, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    std::int64_t t = top_.value.load(std::memory_order_relaxed);

    if (t > b) {
      // Already empty; undo the reservation.
      bottom_.value.store(b + 1, std::memory_order_relaxed);
      return std::nullopt;
    }
    T value = buf->get(b);
    if (t == b) {
      // Last element: race the thieves for it via top.
      const bool won = top_.value.compare_exchange_strong(
          t, t + 1, std::memory_order_seq_cst, std::memory_order_relaxed);
      bottom_.value.store(b + 1, std::memory_order_relaxed);
      if (!won) {
        return std::nullopt;
      }
    }
    return value;
  }

  // Any thread. Takes the oldest element (FIFO); returns nothing when the
  // deque looks empty or the race for the element was lost.
  std::optional<T> steal() {
    std::int64_t t = top_.value.load(std::memory_order_acquire);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    const std::int64_t b = bottom_.value.load(std::memory_order_acquire);
    if (t >= b) {
      return std::nullopt;
    }
    // Loading the buffer after bottom_ is safe although the canonical
    // formulation loads it first: the acquire load of bottom_ synchronizes
    // with the release fence in push(), which is sequenced after grow()'s
    // buffer_ store, so this load cannot observe a ring older than the one
    // that produced the observed bottom.
    ring* buf = buffer_.value.load(std::memory_order_acquire);
    T value = buf->get(t);
    if (!top_.value.compare_exchange_strong(
            t, t + 1, std::memory_order_seq_cst, std::memory_order_relaxed)) {
      return std::nullopt;
    }
    return value;
  }

  // Racy snapshot; exact only while the deque is quiescent.
  std::size_t size() const noexcept {
    const std::int64_t b = bottom_.value.load(std::memory_order_relaxed);
    const std::int64_t t = top_.value.load(std::memory_order_relaxed);
    return b > t ? static_cast<std::size_t>(b - t) : 0;
  }

  bool empty() const noexcept { return size() == 0; }

  std::size_t capacity() const noexcept {
    return buffer_.value.load(std::memory_order_relaxed)->capacity();
  }

 private:
  static std::size_t checked_capacity(std::size_t requested) {
    if (requested > std::numeric_limits<std::size_t>::max() / 2) {
      throw std::length_error("wsq initial capacity too large");
    }
    return std::bit_ceil(std::max<std::size_t>(requested, std::size_t{2}));
  }

  struct ring {
    explicit ring(std::size_t cap)
        : mask(cap - 1), slots(new std::atomic<T>[cap]) {
      assert(std::has_single_bit(cap) && "ring capacity must be a power of 2");
    }

    std::size_t capacity() const noexcept { return mask + 1; }

    void put(std::int64_t index, T value) noexcept {
      slots[static_cast<std::size_t>(index) & mask].store(
          value, std::memory_order_relaxed);
    }

    T get(std::int64_t index) const noexcept {
      return slots[static_cast<std::size_t>(index) & mask].load(
          std::memory_order_relaxed);
    }

    std::size_t mask;
    std::unique_ptr<std::atomic<T>[]> slots;
  };

  // Owner only. Doubles the ring and publishes it; the old ring is retired,
  // not freed, because thieves loaded before the publish may still read it.
  // Failure ordering: everything that can throw does so before any state
  // changes, so an exception leaves the deque exactly as it was.
  ring* grow(ring* old_ring, std::int64_t top, std::int64_t bottom) {
    if (old_ring->capacity() > std::numeric_limits<std::size_t>::max() / 2) {
      throw std::length_error("wsq capacity cannot double further");
    }
    retired_.reserve(retired_.size() + 1);
    auto bigger = std::make_unique<ring>(old_ring->capacity() * 2);
    for (std::int64_t i = top; i < bottom; ++i) {
      bigger->put(i, old_ring->get(i));
    }
    retired_.emplace_back(old_ring);  // Cannot throw: capacity reserved.
    buffer_.value.store(bigger.get(), std::memory_order_release);
    return bigger.release();
  }

  padded<std::atomic<std::int64_t>> top_{};
  padded<std::atomic<std::int64_t>> bottom_{};
  padded<std::atomic<ring*>> buffer_;
  std::vector<std::unique_ptr<ring>> retired_;  // Owner only.
};

}  // namespace taskloom
