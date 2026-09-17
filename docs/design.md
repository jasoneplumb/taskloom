# taskloom design notes

taskloom is a small header-only C++20 concurrency library centered on a
work-stealing deque (`wsq`), with supporting modules it depends on or that
share its conventions: `atomic` (padding and lock-free guarantees), `rwlock`
(a reader-writer spinlock), `event` (dependency-graph completion nodes),
`stats` (streaming statistics), and `hw` (hardware constants and primitives).
This document records the design trade-offs and the measurements behind them.

## wsq: the work-stealing deque

`wsq` implements the Chase-Lev deque with the fence placement of the
weak-memory formulation (Le, Pop, Cohen, Nardelli, "Correct and Efficient
Work-Stealing for Weak Memory Models", PPoPP 2013). One owner thread pushes
and pops at the bottom; any number of thieves steal from the top. Owner
operations are wait-free except when the ring grows; steal is lock-free and
may fail spuriously when it loses a race, which callers treat as "pick
another victim".

Three decisions are worth defending explicitly.

**64-bit monotonic indices.** `top` and `bottom` are `int64_t` and only ever
increase (apart from `pop`'s transient reservation). At one billion
operations per second an `int64_t` index wraps after roughly 290 years, so
`empty()` and `size()` need no wraparound handling, and the top counter's
monotonicity doubles as ABA protection for the steal CAS.

**Retire-list reclamation.** Growth allocates a doubled ring, copies the
live range, publishes the new ring with a release store, and retires the old
ring into a list owned by the deque. A thief that loaded the old ring
pointer just before the publish can still be reading it, so freeing it at
that moment would be a use-after-free. The alternatives are hazard pointers
or epoch reclamation, which reclaim earlier but add per-operation overhead
to the steal path and considerable complexity. Retiring trades peak memory
(old rings live until the deque is destroyed, at most 2x the largest ring in
total) for a steal path with zero reclamation bookkeeping. For a
task-scheduler deque whose rings are small and whose lifetime is a worker
thread's lifetime, that trade is one-sided.

**Atomic element slots.** Elements are stored in `std::atomic<T>` slots, and
`T` is constrained to trivially copyable, lock-free types (pointers or small
handles). A thief may read a slot concurrently with the owner republishing
it after a wraparound; the steal CAS rejects the stale read, but the read
itself must still be well-defined. Making the slots atomic makes every
shared access in the structure an atomic operation, which also means
ThreadSanitizer observes the full synchronization graph with no fence
blind spots.

## rwlock: seq_cst where release/acquire cannot work

`rwlock` is a reader-writer spinlock: readers announce themselves by
incrementing a counter and back out if a writer holds or is claiming the
flag; a writer claims the flag and drains the readers already inside. The
two counters live on separate false-sharing ranges so reader traffic does
not invalidate the writer flag's cache line, and the writer path spins
test-and-test-and-set.

The load-bearing decision is the memory ordering. Acquisition is an
announce-then-check protocol across two variables, which is the
store-buffering litmus shape: each side writes its own variable and reads
the other's. Release/acquire ordering, including acq_rel on the RMWs,
still admits the execution in which both sides read the other's
pre-announcement value, because in that execution no synchronizes-with edge
exists in either direction. Mutual exclusion needs the single total order
of seq_cst on the announce and check operations; whichever announcement
comes later in that order, the matching check is guaranteed to observe the
other side. Unlocks and back-outs only publish data and stay release. On
total-store-order hardware the seq_cst loads cost the same as acquire
loads, so the correctness margin is close to free there, and on weakly
ordered machines it is simply required.

`rwlock` meets the SharedLockable and Lockable requirements and composes
with `std::shared_lock` and `std::unique_lock`. It spins, so it is for
short critical sections; a lock held across blocking work belongs to
`std::shared_mutex` instead.

## event: at-most-once completion with racy edge addition

`event` is a dependency-graph node that fires `on_ready()` when its last
prerequisite completes. Edges may be added concurrently with completions.
The complete-versus-register race is resolved under a plain `std::mutex`: a
registration that finds the prerequisite already complete is notified
immediately instead of being lost, and completion swaps the dependents out
and notifies outside the lock so an `on_ready()` that builds more graph
cannot deadlock. A mutex rather than `rwlock` is deliberate: the dependents
vector cannot accept concurrent inserts on a shared lock, so a reader-writer
split buys nothing here.

The counter alone cannot make firing exactly-once, because a registration
landing between two zero crossings can produce a second crossing. A latch
makes firing at-most-once per cycle; adding an edge to a node that may
already have fired is documented as a contract violation, suppressed by the
latch in release builds and asserted in debug builds. `on_ready()` is
`noexcept` by signature: an exception escaping it would strand the
remaining dependents of the completing prerequisite.

## stats: streaming moments that survive aggregation

`stats` keeps count, total, mean, and the Welford M2 accumulator in constant
space. `merge()` uses the parallel combination formula (Chan, Golub,
LeVeque), so per-thread instances aggregate into exactly what a single
stream would have produced; a merge that combined counts and means but
dropped the M2 term would silently report wrong variance, which is the
failure mode the tests pin down. The merge factor is evaluated as
(nA / N) * nB to keep intermediates small for single-precision
instantiations, and the value type is constrained to floating point because
an integer mean corrupts from the second sample onward.

## hw: constants as upper bounds, primitives as hints

`cache_line_bytes` is 128 on arm64 and 64 elsewhere. That is a deliberate
upper bound rather than a hardware description: some arm64 cores use
128-byte lines and most server cores use 64, and padding against
destructive interference wants the worst case. `false_sharing_bytes` is two
lines, which also defeats adjacent-line prefetching. `cpu_pause()` uses
`isb` on arm64 because the architectural yield hint retires as a no-op on
most cores, while `isb` stalls a few cycles and gives spin loops a real
backoff; this matches the choice made by Rust's `std::hint::spin_loop`.
`timestamp()` is a non-serializing tick counter for aggregate timing, not
for bracketing single instructions.

## Measurements

Methodology: every benchmark is fixed work with a conservation check. The
clock starts when the workload starts (production for the deque benchmarks,
a start gate for the lock benchmarks) and stops when every produced item
has been consumed exactly once; a benchmark that cannot prove conservation
exits nonzero instead of printing a number. The steal workload is one owner
producing 2,000,000 items and popping every 64th, with N thieves stealing
continuously; the ring is pre-sized so growth allocation stays out of the
window. The baseline structure is a mutex-guarded `std::deque` with the
same API shape (owner at the back, thieves at the front) on the identical
workload, which is the structure a task scheduler would otherwise reach
for.

One machine, one data point: arm64 macOS (8 logical cores), Apple clang,
Release, C++20. Numbers below are single runs of `bench/wsq_bench` and
`bench/rwlock_bench`; treat them as representative of this machine, not as
a cross-platform claim.

| workload            | wsq (Mitems/s) | locked deque (Mitems/s) | ratio |
| ------------------- | -------------: | ----------------------: | ----: |
| owner push/pop      |          132.7 |                    20.2 |  6.6x |
| steal, 1 thief      |           24.7 |                     8.8 |  2.8x |
| steal, 2 thieves    |           16.3 |                     4.2 |  3.9x |
| steal, 4 thieves    |           11.9 |                     2.3 |  5.1x |

The claim this table supports: on this machine and workload, the
work-stealing deque sustains 2.8x to 5.1x the throughput of a mutex-guarded
`std::deque` under contention, and the advantage grows with thief count,
because thieves contend on a single CAS word instead of serializing every
operation, owner included, through one lock. Absolute throughput falls for
both structures as thieves are added; the workload is a single producer, so
added thieves add contention, not capacity.

| readers | rwlock (Macq/s) | std::shared_mutex (Macq/s) |
| ------: | --------------: | -------------------------: |
|       1 |            80.0 |                       15.3 |
|       2 |            33.7 |                        7.7 |
|       4 |            30.1 |                        6.5 |

The spinlock's advantage on this read-heavy microbenchmark (one occasional
writer, uncontended-to-lightly-contended shared acquisitions) reflects what
it omits: no syscall path, no fairness machinery, no blocking. The same
omissions are why it is the wrong tool once critical sections block or hold
times grow.

Reproduce with:

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/bench/wsq_bench
./build/bench/rwlock_bench
```
