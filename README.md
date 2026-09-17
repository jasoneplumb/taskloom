# taskloom

[![CI](https://github.com/jasoneplumb/taskloom/actions/workflows/ci.yml/badge.svg?branch=mainline)](https://github.com/jasoneplumb/taskloom/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.cppreference.com/w/cpp/20)

A small header-only C++20 concurrency library centered on a work-stealing deque, with the supporting modules a task scheduler needs around one.

## Modules

- `taskloom/wsq.hpp`: Chase-Lev work-stealing deque: one owner pushes and pops, any number of thieves steal, lock-free stealing with safe ring growth
- `taskloom/rwlock.hpp`: reader-writer spinlock with a seq_cst acquisition protocol; composes with `std::shared_lock` and `std::unique_lock`
- `taskloom/event.hpp`: dependency-graph completion nodes: a node fires once its last prerequisite completes, with race-safe edge addition
- `taskloom/stats.hpp`: constant-space streaming statistics whose merge preserves variance across threads
- `taskloom/atomic.hpp`: `padded<T>` false-sharing isolation and lock-free compile-time guarantees
- `taskloom/hw.hpp`: cache-line and page constants, spin hint, tick counter

## Quick start

```cmake
include(FetchContent)
FetchContent_Declare(taskloom
  GIT_REPOSITORY https://github.com/jasoneplumb/taskloom.git
  GIT_TAG mainline
)
FetchContent_MakeAvailable(taskloom)
target_link_libraries(app PRIVATE taskloom::taskloom)
```

```cpp
#include <taskloom/wsq.hpp>

taskloom::wsq<Task*> queue;

// Owner thread:
queue.push(task);
if (auto mine = queue.pop()) { run(*mine); }

// Any other worker:
if (auto stolen = queue.steal()) { run(*stolen); }
```

## Building and testing

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Tests run under ThreadSanitizer with `-DTASKLOOM_SANITIZE=thread`; CI covers macOS and Linux plus a TSan job. Benchmarks live in `bench/` and are conservation-checked: a reported number is a verified number.

## Design

Trade-off rationale and same-machine measurements, including the comparison against a mutex-guarded `std::deque`, are in [docs/design.md](docs/design.md).

## License

[MIT](LICENSE)
