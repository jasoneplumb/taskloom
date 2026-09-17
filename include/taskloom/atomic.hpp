#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "hw.hpp"

// Atomics convention: this library uses std::atomic directly with explicit
// memory_order arguments at every call site; there are no wrapper macros.
// This header carries what that convention does not already provide: a
// padding wrapper against false sharing, and compile-time guarantees that
// the atomic types the other modules build on are lock-free.

namespace taskloom {

// Keeps its value on a private false-sharing range so writers on adjacent
// data never invalidate the holder's cache line. Intended for hot,
// independently written variables (counters, queue indices, per-thread
// slots in an array).
template <typename T>
struct alignas(false_sharing_bytes) padded {
  static_assert(sizeof(T) <= false_sharing_bytes,
                "padded<T> isolates small hot variables; a T larger than the "
                "false-sharing range already spans multiple ranges");
  static_assert(alignof(T) <= false_sharing_bytes,
                "an over-aligned T would defeat the fixed padding size");
  T value{};
};

static_assert(alignof(padded<std::atomic<std::uint64_t>>) ==
              false_sharing_bytes);
static_assert(sizeof(padded<char>) == false_sharing_bytes);

// The concurrency modules assume these are lock-free; a platform where they
// are not would silently serialize every operation through a lock, so fail
// the build instead.
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<void*>::is_always_lock_free);

}  // namespace taskloom
