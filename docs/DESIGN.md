# MDP Technical Design Document

**Audience:** Senior C++ engineers, quant developers, and technical hiring managers.  
**Scope:** Design rationale for the non-obvious decisions in the Market Data Processor codebase. For a feature overview, see [README.md](../README.md).

---

## 1. Why SPSC Ring Buffer?

### The Access Pattern

Every stage in the MDP pipeline has exactly one producer and one consumer. `FeedSimulator` writes ticks; `TickParser` reads them. `TickParser` then writes processed ticks; `Normalizer` reads them. This **single-producer / single-consumer (SPSC)** invariant is established by construction and never violated at runtime.

### The Cost of the Naïve Alternative

The textbook alternative — `std::queue<MarketTick>` protected by `std::mutex` — has three compounding costs:

1. **Lock acquisition.** Every `push()` and `pop()` requires an exclusive lock. On a modern ARM core, an uncontested `pthread_mutex_lock` / `pthread_mutex_unlock` round-trip costs ~20–50 ns. At 350 M ticks/s, this alone would cap throughput below 20 M ticks/s.

2. **False sharing.** `std::queue` is typically implemented as a `std::deque`, whose internal node pointers are adjacent in memory. When the producer modifies the back pointer and the consumer modifies the front pointer, both modifications land on the same or adjacent cache lines. Every write by one core causes a **cache line invalidation** seen by the other core — forcing a cache miss (100–300 cycles on Apple M-series) on every operation.

3. **Dynamic allocation.** `std::deque` allocates heap chunks as the queue grows. On the hot path, `malloc`/`free` introduces non-deterministic latency spikes incompatible with real-time market data processing.

### The SPSC Solution

`RingBuffer<T, N>` eliminates all three costs:

- **No lock.** The producer writes to `head_` and reads `tail_`; the consumer writes to `tail_` and reads `head_`. Because writes to distinct variables never race, no mutex is needed.
- **No false sharing.** `head_` and `tail_` are each wrapped in an `alignas(64) AlignedIndex` struct, placing them on separate cache lines. The producer and consumer never invalidate each other's hot cache line.
- **No allocation.** The buffer is a fixed-size `std::array<T, N>` allocated once at construction.

**Measured result:** 351.6 M sequential push+pop operations per second (~2.8 ns/round-trip). The true SPSC cross-core throughput is ~25 M items/s, which reflects the irreducible cost of cache coherence traffic between two physical cores.

### Buffer Sizing

`N` must be a power of two (`PowerOfTwo<N>` concept, checked at compile time). This allows slot indexing via bitmask (`counter & (N - 1)`) rather than modulo division, which maps to a single `AND` instruction instead of a potentially multi-cycle `UDIV`. The high-capacity `RB16K` (16 384 slots, ~640 KiB) sits between FeedSimulator and TickParser to absorb burst production; the downstream `RB4K` (4 096 slots, ~160 KiB) is sufficient given the normalizer's deduplication reduces effective throughput.

---

## 2. Memory Ordering Rationale

The SPSC ring buffer uses precisely the minimum memory ordering required for correctness — no more, no less. Here is the exact analysis.

### The Acquire/Release Pair

In `try_push()`:
```cpp
// Own index — only producer writes it; local cache is always fresh.
const std::size_t head = head_.value.load(std::memory_order_relaxed);

// Other index — must synchronise-with the consumer's release store.
const std::size_t tail = tail_.value.load(std::memory_order_acquire);

buffer_[head & (N-1)] = std::move(item);

// Publish: ensures buffer write is visible before head increment.
head_.value.store(head + 1, std::memory_order_release);
```

In `try_pop()`:
```cpp
const std::size_t tail = tail_.value.load(std::memory_order_relaxed);

// Must see the element the producer wrote before incrementing head.
const std::size_t head = head_.value.load(std::memory_order_acquire);

out = std::move(buffer_[tail & (N-1)]);

// Free the slot: ensures move-out is complete before producer reuses.
tail_.value.store(tail + 1, std::memory_order_release);
```

### Why Not `relaxed` for the Cross-Index Load?

If the consumer loaded `head_` with `relaxed`, the C++20 memory model would permit the compiler or CPU to reorder the `out = std::move(buffer_[...])` read *before* the load of `head_`. The consumer could observe a stale `head_`, conclude the buffer is non-empty, and read from a slot the producer has not yet written — a **data race**. The `acquire` on the cross-index load establishes a happens-before edge: all writes by the producer *before* its `release` store to `head_` are visible to the consumer after its `acquire` load of `head_`.

### Why Not `seq_cst`?

`seq_cst` imposes a total order on all `seq_cst` operations across all threads — essentially a full memory fence on every operation. On x86-64, `seq_cst` stores are implemented with `MFENCE` or `XCHG`, which serialises the store buffer. On ARM (Apple M-series), `seq_cst` loads require an `LDAR` instruction rather than a plain `LDR`. For operations running hundreds of millions of times per second, this overhead is measurable. The acquire/release pair provides the same correctness guarantee with less hardware overhead.

### Monitoring Helpers Use `seq_cst`

`empty()`, `full()`, and `size()` use `seq_cst` because they are called from monitoring threads — not the hot path — and need a globally consistent snapshot. A `relaxed` load of both `head_` and `tail_` in sequence could observe them from different points in time, giving a meaningless result. The `seq_cst` constraint on both loads ensures they are ordered with respect to each other.

---

## 3. CRTP ThreadBase Pattern

### The Problem with Virtual Dispatch for Thread Management

A pipeline stage that inherits a `start()`/`stop()`/`run()` interface via virtual dispatch incurs a virtual call overhead on every lifecycle event. More critically, it risks the **destructor problem**: if a base class destructor calls a virtual `stop()`, the derived class has already been destroyed by the time the base destructor runs, making the call undefined behaviour. This is the canonical "pure virtual function called" crash.

### CRTP Avoids Virtual Dispatch on the Hot Path

`ThreadBase` uses the Curiously Recurring Template Pattern: the concrete stage inherits `ThreadBase<Derived>` and overrides `run(StopToken)` as a non-virtual method. `ThreadBase::start()` creates a `std::jthread` that calls `derived().run(st)` — resolved at compile time with zero vtable overhead.

```cpp
template <typename Derived>
class ThreadBase {
public:
    void start() {
        thread_ = std::jthread([this](std::stop_token st) {
            static_cast<Derived*>(this)->run(std::move(st));
        });
    }
    void stop() { thread_.request_stop(); thread_.join(); }
};
```

### `std::jthread` + `std::stop_token` vs Manual `atomic<bool>`

The traditional approach — a `std::atomic<bool> running_` flag checked in the loop — has two subtle failure modes:
1. **Spurious termination:** if `running_` is stored before the thread fully initialises, the loop may exit immediately.
2. **Missed wakeup:** if the consumer is sleeping in `sleep_until()` when `running_` is set to false, it can block for up to one full tick interval before noticing.

`std::jthread` with `std::stop_token` solves both. `request_stop()` is atomic and composable; `stop_requested()` integrates with `std::condition_variable_any` for interruptible waits. The destructor of `std::jthread` calls `request_stop()` and `join()` automatically, making the pattern RAII-safe by default.

---

## 4. FeedSimulator Price Model

### Why Independent Random Walks Fail

The naive simulation drives two independent Gaussian random walks, one for bid and one for ask:

```cpp
bid_price_ += gaussian(0, σ);
ask_price_ += gaussian(0, σ);
```

Two independent geometric Brownian motions will drift apart or converge with probability 1 over time. The spread `ask - bid` performs its own random walk with no mean-reversion, and the sign is not guaranteed. In practice, after ~200 ms of simulation at 10 kHz, the crossed-book rejection rate reaches 30–40%.

### The Geometric Brownian Motion + Ornstein-Uhlenbeck Model

The corrected model separates price dynamics into two orthogonal processes:

**Mid-price (GBM):**
```
dS = σ · S · dW
```
The mid-price follows a multiplicative random walk (proportional to current price). This ensures price increments scale with magnitude — a $1 move in AAPL and a $1 move in BTCUSD have very different significance. Volatility is expressed as a fraction: `σ = mid_price × 0.001` per tick.

**Spread (Ornstein-Uhlenbeck mean reversion):**
```
dα = κ(α* - α) dt + ε · dW_spread
```
where `α` is the current spread, `α*` is the target spread (0.01 for equities, 5.0 for BTCUSD), and `κ = 0.1` is the reversion speed. The spread is clamped to `[SPREAD_MIN, SPREAD_MAX]` after each step.

**Bid/Ask construction:**
```
bid = mid - spread/2
ask = mid + spread/2
```

**Mathematical guarantee:** `mid > 10.0` (lower bound enforced by `std::clamp`), `spread ≥ SPREAD_MIN = 0.001`. Therefore `bid = mid - 0.0005 > 9.9995 > 0`, and `ask = bid + spread > bid`. The invariant `ask > bid > 0` is unconditional.

---

## 5. NormalizerStats — Atomics vs Mutex

### The Original Data Race

A common mistake in multi-threaded statistics collection is protecting the *read* path with a mutex while leaving the *write* path unprotected:

```cpp
// Thread 1 (Normalizer hot path) — no lock:
++ticks_forwarded_;

// Thread 2 (monitoring thread) — with lock:
{
    std::lock_guard lock(mutex_);
    snapshot.ticks_forwarded = ticks_forwarded_;
}
```

This is **not safe**. The mutex on the reader side alone does not synchronise with the unprotected write on thread 1. Thread 2 has no happens-before relationship to thread 1's write, and the compiler is free to cache `ticks_forwarded_` in a register across the lock boundary. This is undefined behaviour under the C++20 memory model and a data race that ThreadSanitizer would correctly flag.

### `std::atomic<uint64_t>` with `memory_order_relaxed`

The correct solution is `std::atomic<uint64_t>`:

```cpp
// Thread 1: single writer, relaxed is sufficient.
ticks_forwarded_.fetch_add(1, std::memory_order_relaxed);

// Thread 2: relaxed load is safe for monotonic counters.
snapshot.ticks_forwarded = ticks_forwarded_.load(std::memory_order_relaxed);
```

For monotonically increasing counters used only for reporting — not for controlling synchronisation between threads — `relaxed` ordering is correct. The counter's value is always valid (no torn reads on 64-bit aligned atomics on arm64 and x86-64) and always monotonically increasing. The observer thread may see a slightly stale value, but for a health check or metrics dashboard, this is acceptable.

### Why `snapshot()` Over Raw Atomic Exposure

Exposing raw atomics through a public API forces callers to reason about memory ordering themselves. The `MetricsCollector::snapshot()` method copies all counters at a single point in time, returns a plain struct, and lets callers use simple value semantics. This separation of "what the atomics mean internally" from "what the metrics mean to a consumer" is the key design principle.

---

## 6. OrderBook Crossed-Book Rejection

### The Invariant

A valid limit order book satisfies at all times:

```
best_ask > best_bid > 0
```

This is the fundamental no-arbitrage condition at the level of the exchange matching engine. If `best_ask ≤ best_bid`, the book is *crossed* — a buy order already in the book is priced higher than the cheapest sell order. In a real exchange, crossed books trigger immediate matching; in a market data consumer, a crossed book always indicates a data quality problem.

### Sources of Crossed Books in Production

1. **Stale feed replay.** If a feed replays historical tick data at the wrong sequence offset, bid and ask updates from different logical times can interleave incorrectly.
2. **Multi-feed aggregation.** When combining quotes from multiple sources (e.g., NYSE and a dark pool), sequencing gaps can cause transient crosses.
3. **FeedSimulator regression.** As demonstrated in Sprint 7C, independent random walks in the simulator caused 30–40% crossed book rates within seconds of startup — resolved by switching to the GBM + OU model.

### Rejection vs Correction

`OrderBook::apply()` rejects crossed updates rather than correcting them:

```cpp
if (delta.side == OrderSide::BID && !asks_.empty()
    && delta.price >= asks_.begin()->first) {
    std::fprintf(stderr, "[BOOK WARN] Crossed book rejected...\n");
    return; // discard
}
```

Silently correcting a crossed book — e.g., by adjusting the incoming bid to `best_ask - epsilon` — would mask the upstream bug that produced the invalid tick. Rejecting and logging makes data quality problems immediately visible. In production, a crossed-book rejection rate above noise (> 0.1%) should page an on-call engineer.

---

## 7. Known Limitations & Future Work

### Performance

**`std::map<double, Level>` for price levels (O(log n)).**  
Production order book implementations use a flat array indexed by price level, a skip list, or a sorted `std::vector` with binary search for the hot path. `std::map` provides O(log n) updates and excellent iterator stability, which is sufficient for a simulation with tens of price levels per symbol, but would not scale to a full NYSE book (thousands of levels, millions of updates per second). Replacing `std::map` with `boost::container::flat_map` or a hand-written level array is the most impactful remaining performance optimisation.

**Single-symbol `OrderBook` has no lock, but `BookProcessor::book()` is not thread-safe under live load.**  
The `book(symbol)` accessor is explicitly documented as post-`stop()` only. Adding a concurrent read path (e.g., a lock-free `snapshot()` accessible from a strategy thread) would require either a reader-writer lock or a double-buffered snapshot pattern.

### Feed Realism

**`FeedSimulator` generates synthetic data only.**  
A production feed adapter would implement `IFeedSource` against a real market data protocol — most commonly **ITCH 5.0** (NASDAQ native binary, ~2 µs latency), **SBE** (Simple Binary Encoding, FIX community standard), or a proprietary WebSocket feed (Binance, Coinbase). The `IFeedSource` interface is deliberately minimal to allow such adapters to be dropped in without changes to the downstream pipeline.

### Infrastructure

**No persistence layer.**  
Ticks are consumed in-memory and discarded after `BookProcessor` applies them. Production systems write a tick store (typically a memory-mapped log file or a time-series database such as kdb+/q or InfluxDB) for replay, backtesting, and regulatory audit.

**No network layer.**  
The current system is a single-process, single-machine library. A production deployment would add a **publish/subscribe** layer — either shared memory (for co-located strategies) or a network transport such as UDP multicast with sequence numbers and gap-fill retransmission for remote consumers.

**No strategy module.**  
The `src/strategy/` directory is scaffolded but empty. The intended design is a pluggable `IStrategy` interface that receives `TopOfBook` updates from `BookProcessor` and emits order intent — a clean separation of market data infrastructure from alpha logic.
