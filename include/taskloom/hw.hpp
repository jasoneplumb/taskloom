#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>

#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#endif

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace taskloom {

// Size used to keep hot variables on separate cache lines. A conservative
// compile-time constant rather than std::hardware_destructive_interference_size,
// which is not available on every supported standard library.
// 128 on arm64 is a deliberate upper bound, not a hardware description:
// some arm64 cores use 128-byte lines while most server cores use 64, and
// padding for interference avoidance wants the worst case.
#if defined(__aarch64__) || defined(_M_ARM64)
inline constexpr std::size_t cache_line_bytes = 128;
#else
inline constexpr std::size_t cache_line_bytes = 64;
#endif

// Padding span that also defeats adjacent-line hardware prefetching.
inline constexpr std::size_t false_sharing_bytes = 2 * cache_line_bytes;

// Runtime page size, queried once and cached.
inline std::size_t page_size() noexcept {
#if defined(_WIN32)
  static const std::size_t cached = [] {
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return static_cast<std::size_t>(info.dwPageSize);
  }();
#else
  static const std::size_t cached = [] {
    const long sc = sysconf(_SC_PAGESIZE);
    return sc > 0 ? static_cast<std::size_t>(sc) : std::size_t{4096};
  }();
#endif
  return cached;
}

// Polite busy-wait hint to the CPU; use inside spin loops.
inline void cpu_pause() noexcept {
#if defined(__x86_64__) || defined(__i386__) || \
    (defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86)))
  _mm_pause();
#elif defined(__aarch64__)
  // Deliberately isb rather than the architectural yield hint: yield is a
  // no-op on most cores, while isb stalls for a few cycles and gives spin
  // loops a real backoff (the same choice as Rust's std::hint::spin_loop).
  asm volatile("isb" ::: "memory");
#elif defined(_M_ARM64)
  __isb(_ARM64_BARRIER_SY);  // Same rationale as the isb branch above.
#else
  // No pause instruction available; let the scheduler deschedule the spinner
  // instead of burning the core at full speed.
  std::this_thread::yield();
#endif
}

// Cheap monotonic timestamp in hardware ticks. Tick frequency differs per
// platform; use only for relative measurements within one process run.
// The counter reads are non-serializing (rdtsc in particular can reorder
// around neighboring instructions); aggregate and loop timings are fine,
// but tight microbenchmarks need rdtscp plus a fence around the sample.
inline std::uint64_t timestamp() noexcept {
#if defined(__x86_64__) || defined(__i386__) || \
    (defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86)))
  return __rdtsc();
#elif defined(__aarch64__)
  // Assumes user-space access to the virtual counter is enabled, as it is
  // on mainstream kernels. Configurations that trap this register are not
  // supported by this build path; the chrono branch below is compile-time
  // only, not a runtime fallback.
  std::uint64_t ticks;
  asm volatile("mrs %0, cntvct_el0" : "=r"(ticks) :: "memory");
  return ticks;
#else
  return static_cast<std::uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

}  // namespace taskloom
