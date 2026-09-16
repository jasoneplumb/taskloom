#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <bit>
#include <taskloom/hw.hpp>

static_assert(std::has_single_bit(taskloom::cache_line_bytes));
static_assert(taskloom::false_sharing_bytes % taskloom::cache_line_bytes == 0);

TEST_CASE("page_size is a plausible power of two") {
  const std::size_t page = taskloom::page_size();
  CHECK(page >= 4096);
  CHECK(std::has_single_bit(page));
  CHECK(page == taskloom::page_size());
}

TEST_CASE("cpu_pause is callable in a loop") {
  for (int i = 0; i < 64; ++i) {
    taskloom::cpu_pause();
  }
  CHECK(true);
}

TEST_CASE("timestamp advances across a busy wait") {
  const std::uint64_t before = taskloom::timestamp();
  for (int i = 0; i < 100000; ++i) {
    taskloom::cpu_pause();
  }
  const std::uint64_t after = taskloom::timestamp();
  CHECK(after > before);
}
