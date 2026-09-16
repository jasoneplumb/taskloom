#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cmath>
#include <cstddef>
#include <random>
#include <taskloom/stats.hpp>
#include <vector>

namespace {

struct batch_result {
  double mean = 0;
  double variance = 0;
  double total = 0;
};

// Two-pass textbook reference the streaming implementation must match.
batch_result batch_reference(const std::vector<double>& samples) {
  batch_result r;
  for (const double s : samples) {
    r.total += s;
  }
  r.mean = r.total / static_cast<double>(samples.size());
  double sum_sq = 0;
  for (const double s : samples) {
    sum_sq += (s - r.mean) * (s - r.mean);
  }
  r.variance = samples.size() > 1
                   ? sum_sq / static_cast<double>(samples.size() - 1)
                   : 0;
  return r;
}

}  // namespace

TEST_CASE("known dataset produces textbook values") {
  taskloom::double_stats s;
  for (const double v : {2.0, 4.0, 4.0, 4.0, 5.0, 5.0, 7.0, 9.0}) {
    s.add(v);
  }
  CHECK(s.count() == 8);
  CHECK(s.total() == doctest::Approx(40.0));
  CHECK(s.mean() == doctest::Approx(5.0));
  CHECK(s.variance() == doctest::Approx(32.0 / 7.0));
  CHECK(s.std_dev() == doctest::Approx(std::sqrt(32.0 / 7.0)));
  CHECK(s.coefficient_of_variation() ==
        doctest::Approx(std::sqrt(32.0 / 7.0) / 5.0));
}

TEST_CASE("merge matches the single-stream result") {
  std::mt19937 rng(12345);
  std::uniform_real_distribution<double> dist(-100.0, 100.0);
  std::vector<double> samples(9001);
  for (double& s : samples) {
    s = dist(rng);
  }

  taskloom::double_stats whole;
  for (const double s : samples) {
    whole.add(s);
  }

  // Three unequal chunks filled independently, then merged: this is the
  // per-thread aggregation shape, and variance must survive it.
  taskloom::double_stats parts[3];
  const std::size_t cuts[4] = {0, 1000, 4500, samples.size()};
  for (int p = 0; p < 3; ++p) {
    for (std::size_t i = cuts[p]; i < cuts[p + 1]; ++i) {
      parts[p].add(samples[i]);
    }
  }
  taskloom::double_stats merged;
  merged.merge(parts[0]);
  merged.merge(parts[1]);
  merged.merge(parts[2]);

  const batch_result ref = batch_reference(samples);
  CHECK(merged.count() == whole.count());
  CHECK(merged.mean() == doctest::Approx(whole.mean()));
  CHECK(merged.variance() == doctest::Approx(whole.variance()));
  CHECK(merged.mean() == doctest::Approx(ref.mean));
  CHECK(merged.variance() == doctest::Approx(ref.variance));
  CHECK(merged.total() == doctest::Approx(ref.total));
}

TEST_CASE("merge edge cases") {
  taskloom::double_stats empty;
  taskloom::double_stats filled;
  filled.add(1.0);
  filled.add(3.0);

  SUBCASE("merging an empty instance changes nothing") {
    taskloom::double_stats copy = filled;
    copy.merge(empty);
    CHECK(copy.count() == 2);
    CHECK(copy.mean() == doctest::Approx(2.0));
    CHECK(copy.variance() == doctest::Approx(2.0));
  }

  SUBCASE("merging into an empty instance adopts the other") {
    taskloom::double_stats target;
    target.merge(filled);
    CHECK(target.count() == 2);
    CHECK(target.mean() == doctest::Approx(2.0));
    CHECK(target.variance() == doctest::Approx(2.0));
  }
}

TEST_CASE("degenerate counts and reset") {
  taskloom::double_stats s;
  CHECK(s.variance() == 0.0);
  CHECK(s.coefficient_of_variation() == 0.0);
  s.add(42.0);
  CHECK(s.mean() == doctest::Approx(42.0));
  CHECK(s.variance() == 0.0);
  s.reset();
  CHECK(s.count() == 0);
  CHECK(s.mean() == 0.0);
  CHECK(s.total() == 0.0);
}
