#pragma once

#include <cmath>
#include <cstdint>
#include <type_traits>

namespace taskloom {

// Constant-space streaming statistics over a sample stream, using Welford's
// update for numerical stability. merge() combines two independently filled
// instances (for example one per worker thread) with the parallel variance
// formula, so aggregated variance matches what a single stream would have
// produced. Not thread-safe; fill one instance per thread and merge.
template <typename Value = double>
class stats {
  static_assert(std::is_floating_point_v<Value>,
                "integer accumulators would corrupt the running mean");

 public:
  void add(Value sample) noexcept {
    ++count_;
    const Value delta = sample - mean_;
    mean_ += delta / static_cast<Value>(count_);
    m2_ += delta * (sample - mean_);
    total_ += sample;
  }

  void merge(const stats& other) noexcept {
    if (&other == this || other.count_ == 0) {
      return;
    }
    if (count_ == 0) {
      *this = other;
      return;
    }
    const std::uint64_t combined = count_ + other.count_;
    const Value delta = other.mean_ - mean_;
    // (nA / N) * nB rather than nA * nB / N: keeps the intermediate small
    // so single-precision instantiations do not round the product.
    m2_ += other.m2_ + delta * delta *
                           ((static_cast<Value>(count_) /
                             static_cast<Value>(combined)) *
                            static_cast<Value>(other.count_));
    mean_ += delta * (static_cast<Value>(other.count_) /
                      static_cast<Value>(combined));
    total_ += other.total_;
    count_ = combined;
  }

  void reset() noexcept { *this = stats{}; }

  std::uint64_t count() const noexcept { return count_; }
  Value total() const noexcept { return total_; }
  Value mean() const noexcept { return mean_; }

  // Sample variance (n - 1 denominator); zero until two samples exist.
  Value variance() const noexcept {
    return count_ > 1 ? m2_ / static_cast<Value>(count_ - 1) : Value{0};
  }

  Value std_dev() const noexcept { return std::sqrt(variance()); }

  // Coefficient of variation, sigma / |mu|; zero when the mean is zero.
  Value coefficient_of_variation() const noexcept {
    return mean_ != Value{0} ? std_dev() / std::abs(mean_) : Value{0};
  }

 private:
  std::uint64_t count_ = 0;
  Value mean_{0};
  Value m2_{0};
  Value total_{0};
};

using double_stats = stats<double>;
using float_stats = stats<float>;

}  // namespace taskloom
