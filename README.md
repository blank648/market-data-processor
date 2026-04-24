# Market Data Processor (MDP)

High-performance C++20 lock-free market data processor with SPSC ring buffers, real-time order book maintenance, and Google Benchmark performance suite.

![C++20](https://img.shields.io/badge/C%2B%2B-20-blue)
![CMake](https://img.shields.io/badge/CMake-3.25%2B-green)
![License](https://img.shields.io/badge/license-MIT-lightgrey)
![Platform](https://img.shields.io/badge/platform-macOS%20arm64-lightgrey)

---

## Architecture Overview

MDP is a four-stage, fully pipelined market data processing system. Each stage runs on a dedicated `std::jthread` and communicates with the next exclusively through a lock-free SPSC ring buffer. There are no mutexes, no condition variables, and no shared state between stages.

```
┌─────────────────────────────────────────────────────────┐
│                   MDP Pipeline                          │
│                                                         │
│  FeedSimulator ──[RB16K]──► TickParser                  │
│       │                          │                      │
│   jthread                    jthread                    │
│   GBM+MRS                   validate                    │
│   price                     sequence                    │
│   model                          │                      │
│                           [RB4K]─┘                      │
│                                │                        │
│                           Normalizer                    │
│                            jthread                      │
│                            dedup                        │
│                                │                        │
│                           [RB4K]─┘                      │
│                                │                        │
│                         BookProcessor                   │
│                            jthread                      │
│                          OrderBook×N                    │
└─────────────────────────────────────────────────────────┘

RB16K = RingBuffer<MarketTick, 16384>   (~640 KiB tick storage)
RB4K  = RingBuffer<MarketTick,  4096>   (~160 KiB tick storage)
```

**Stage responsibilities:**

| Stage | Thread Role | Key Operation |
|---|---|---|
| `FeedSimulator` | Producer | GBM price model, configurable tick-rate (Hz) |
| `TickParser` | Consumer + Producer | Validates prices, assigns sequence numbers |
| `Normalizer` | Consumer + Producer | Deduplicates identical consecutive ticks per symbol |
| `BookProcessor` | Consumer | Maintains one `OrderBook` per symbol |

---

## Key Design Decisions

- **SPSC Ring Buffer over `std::queue` + mutex.** The pipeline's access pattern — one writer, one reader per queue — is exactly the SPSC invariant. A mutex-protected `std::queue` would serialize producer and consumer on every operation and thrash the cache. The lock-free design achieves 351 M push+pop round-trips per second with deterministic O(1) latency. See `docs/DESIGN.md` for the memory-ordering rationale.

- **`alignas(64)` cache-line isolation for `head_` and `tail_`.** The producer writes `head_` and reads `tail_`; the consumer does the opposite. If they share a cache line, each write by one core invalidates the other's cache entry — a *false-sharing* stall measured in hundreds of nanoseconds per operation. Padding each index to its own 64-byte line eliminates this entirely.

- **`memory_order_acquire` / `memory_order_release` — not `seq_cst`.** The happens-before guarantee only requires an acquire load on the *other* index and a release store on the *own* index. Using `seq_cst` would impose a full memory fence on every push and pop — unnecessary overhead on a system where correctness requires only the acquire/release pair.

- **CRTP `ThreadBase` with `std::jthread` + `std::stop_token`.** All pipeline components inherit `ThreadBase`, which encapsulates jthread lifecycle management (start, stop, cooperative cancellation). Using `std::jthread` eliminates the manual `atomic<bool> running_` pattern and guarantees the thread is joined in the destructor, preventing the most common threading pitfall: destroying a running thread object.

- **GBM mid-price with Ornstein-Uhlenbeck spread.** `FeedSimulator` drives a geometric Brownian motion for the mid-price and a mean-reverting (OU) process for the bid-ask spread. This guarantees `ask > bid > 0` at all times — the mathematical invariant that eliminates the "crossed book" rejections that a naive dual-random-walk produces. In a 500 ms health check run, the drop rate is consistently **0.00%**.

- **Atomic monotonic counters with `memory_order_relaxed`.** Each pipeline stage tracks ticks processed, rejected, and dropped via `std::atomic<uint64_t>`. For write-once-read-rarely counters like these, `relaxed` ordering is correct and avoids the fence overhead of stronger orderings. A `snapshot()` method copies all counters atomically at a point in time for external consumption by `MetricsCollector`.

- **Crossed-book rejection in `OrderBook`.** When a new bid price ≥ the current best ask (or a new ask ≤ the current best bid), the update is rejected rather than silently accepted. In production, a crossed book signals either a stale feed, a replay error, or an imminent arbitrage; the correct response is always to log and discard, not to corrupt the book state.

---

## Project Structure

```
market-data-processor/
│
├── CMakeLists.txt              # Root build file; FetchContent wiring
├── cmake/
│   └── FetchDependencies.cmake # Declarative dep pinning (gtest, gbench, spdlog, json)
│
├── src/
│   ├── core/                   # Header-only: MarketTick, RingBuffer, ThreadBase
│   │   └── include/core/
│   │       ├── MarketTick.hpp  # 40-byte POD tick struct, trivially copyable
│   │       ├── RingBuffer.hpp  # SPSC lock-free ring buffer template
│   │       └── ThreadBase.hpp  # CRTP base: jthread lifecycle management
│   │
│   ├── feed/                   # FeedSimulator — synthetic market data source
│   │   ├── include/feed/
│   │   │   ├── FeedConfig.hpp  # Config: symbols, tick_rate_hz, volatility
│   │   │   ├── FeedSimulator.hpp
│   │   │   └── IFeedSource.hpp # Abstract interface for pluggable feed sources
│   │   └── src/
│   │       └── FeedSimulator.cpp # GBM + OU spread price model, rate control
│   │
│   ├── processing/             # TickParser + Normalizer
│   │   ├── include/processing/
│   │   │   ├── TickParser.hpp
│   │   │   └── Normalizer.hpp  # NormalizerStats with atomic counters
│   │   └── src/
│   │       ├── TickParser.cpp  # Validates price bounds, assigns sequence numbers
│   │       └── Normalizer.cpp  # Deduplicates by last-price cache per symbol
│   │
│   ├── book/                   # OrderBook + BookProcessor
│   │   ├── include/book/
│   │   │   ├── BookTypes.hpp   # OrderSide, PriceLevel, TopOfBook, BookDelta
│   │   │   ├── OrderBook.hpp   # std::map<double, Level> bid/ask sides
│   │   │   └── BookProcessor.hpp
│   │   └── src/
│   │       ├── OrderBook.cpp   # apply(), snapshot(), crossed-book guard
│   │       └── BookProcessor.cpp # Manages N OrderBooks, EMA side heuristic
│   │
│   ├── infra/                  # Cross-cutting infrastructure
│   │   └── include/infra/
│   │       ├── Logger.hpp      # spdlog factory wrapper
│   │       └── MetricsCollector.hpp # Unified snapshot of all component counters
│   │
│   └── tools/                  # Standalone executables
│       └── health_check.cpp    # mdp-health CLI: human-readable + --json mode
│
├── tests/                      # GoogleTest suite (41 tests)
│   ├── core_tests.cpp          # RingBuffer, MarketTick, ThreadBase unit tests
│   ├── book_tests.cpp          # OrderBook, BookProcessor, edge cases
│   ├── integration_test.cpp    # Full 4-stage pipeline integration tests
│   └── infra_tests.cpp         # MetricsCollector unit tests
│
├── benchmarks/                 # Google Benchmark suite (9 benchmarks)
│   ├── ring_buffer_bench.cpp   # Sequential, burst, SPSC concurrent
│   ├── tick_parser_bench.cpp   # Throughput + single-tick latency
│   ├── normalizer_bench.cpp    # Unique path + dedup path
│   ├── pipeline_bench.cpp      # End-to-end throughput + BookSnapshot latency
│   └── baseline_results.json  # Committed performance baseline
│
└── docs/
    └── DESIGN.md               # Technical design rationale for senior reviewers
```

---

## Build Instructions

### Prerequisites

- macOS arm64 (Apple Silicon) or Linux x86-64
- Clang ≥ 17 or GCC ≥ 14 with C++20 support
- CMake ≥ 3.25
- Internet access (FetchContent pulls dependencies on first configure)

### Debug — AddressSanitizer + UndefinedBehaviorSanitizer

```bash
cmake -B build-debug -S . -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug --parallel
```

### Release — Fully Optimised (`-O3 -march=native`)

```bash
cmake -B build-release -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --parallel
```

### TSan — ThreadSanitizer (data race detection)

```bash
cmake -B build-tsan -S . -DCMAKE_BUILD_TYPE=Tsan
cmake --build build-tsan --parallel
```

> **Note:** TSan and ASan are mutually exclusive build types. The TSan build uses `-O1` so the sanitizer can instrument memory accesses effectively. Do not combine `-DCMAKE_BUILD_TYPE=Debug` with manual `-fsanitize=thread` flags.

---

## Running Tests

All three build variants use the same `ctest` invocation:

```bash
# Release (fastest — use for CI)
cd build-release && ctest --output-on-failure && cd ..

# Debug (ASan + UBSan — use for development)
cd build-debug && ctest --output-on-failure && cd ..

# TSan (data race detection — requires native arm64, not virtualised)
cd build-tsan && ctest --output-on-failure 2>&1 | tee tsan_output.txt && cd ..
```

**Expected output (all builds):**
```
100% tests passed, 0 tests failed out of 41
Total Test time (real) =   2.12 sec
```

> **TSan on macOS:** TSan requires a fully native macOS arm64 environment. Running inside a virtualised or emulated arm64 environment (e.g., GitHub Actions free tier) will produce segfaults at test binary startup due to missing Mach-O interceptor signatures. This is a platform constraint, not a code defect.

---

## Running Benchmarks

```bash
# Run the full benchmark suite (Release build only)
./build-release/benchmarks/mdp-benchmarks

# Filter to a specific stage
./build-release/benchmarks/mdp-benchmarks --benchmark_filter="BM_Pipeline"

# Write results to JSON for baseline comparison
./build-release/benchmarks/mdp-benchmarks \
    --benchmark_out=benchmarks/baseline_results.json \
    --benchmark_out_format=json \
    --benchmark_min_time=1.0s
```

### Performance Baseline — Apple M-series (arm64, Release)

| Benchmark | Result | Notes |
|---|---|---|
| RingBuffer sequential push+pop | **351.6 M items/s** | Single-threaded baseline, ~3 ns/round-trip |
| RingBuffer burst (4096 ticks) | ~467 M items/s | Fill then drain a full 4K buffer |
| RingBuffer SPSC concurrent | ~25.7 M items/s | True producer + consumer on separate cores |
| TickParser throughput (10K batch) | **4.4 M ticks/s** | End-to-end parse + sequence assign |
| TickParser single-tick latency | ~200 ns | Push-to-pop round trip, single tick |
| Normalizer unique ticks (1K batch) | — | All-forward path, zero dedup |
| Normalizer duplicate ticks (1K batch) | — | All-dedup path, identical consecutive |
| Pipeline @ 10 kHz (3 symbols) | **30,002 book_ticks/s** | Full 4-stage pipeline under live load |
| BookSnapshot latency | **~200–400 ns** | `snapshot()` under 10 kHz live load |

The SPSC concurrent benchmark running at ~25 M items/s vs ~352 M sequential reflects the cost of true cross-core synchronisation — the acquire/release fence pair and cache line transfer from producer core to consumer core. This is the theoretical throughput ceiling for the pipeline under heavy load.

---

## Health Check CLI

The `mdp-health` tool spins up the full 4-stage pipeline for 500 ms, collects a `MetricsCollector` snapshot, and reports pipeline health. It is intended for smoke testing production deployments or CI health gates.

### Human-Readable Mode (default)

```bash
./build-release/src/tools/mdp-health
```

```
=== MDP Health Check ===
Runtime          : 500ms
Feed published   : 15141 ticks
Feed dropped     : 0
Parser processed : 15141 ticks
Parser rejected  : 0
Norm forwarded   : 15141 ticks
Norm deduplicated: 0
Books active     : 3
Book ticks       : 15141
--- Derived ---
Feed drop rate   : 0.00%
Parser reject    : 0.00%
Norm dedup rate  : 0.00%
Status           : HEALTHY
```

### JSON Mode (`--json` flag)

```bash
./build-release/src/tools/mdp-health --json
```

```json
{
  "book": {
    "books_active": 3,
    "ticks_processed": 15141
  },
  "derived": {
    "feed_drop_rate": 0.0,
    "norm_dedup_rate": 0.0,
    "parser_reject_rate": 0.0
  },
  "feed": {
    "ticks_dropped": 0,
    "ticks_published": 15141
  },
  "normalizer": {
    "ticks_deduplicated": 0,
    "ticks_forwarded": 15141,
    "ticks_reordered": 0
  },
  "parser": {
    "ticks_processed": 15141,
    "ticks_rejected": 0
  },
  "snapshot_time_ns": 351744789657916
}
```

The JSON output is written to `stdout`, cleanly separated from any `spdlog` output on `stderr`. This makes it directly pipeable to `jq`, monitoring agents, or CI health-gate scripts.

---

## Thread Safety Model

MDP enforces a strict **single-producer / single-consumer (SPSC)** invariant on every ring buffer: at runtime, exactly one thread calls `try_push()` on a given buffer, and exactly one different thread calls `try_pop()`. This invariant is documented in `RingBuffer.hpp` and enforced by construction — each buffer is owned by its adjacent pipeline stages and never shared with a third thread. Because SPSC queues require no CAS loop and no mutex, the hot path reduces to an atomic load, an array write or read, and an atomic store — all wait-free with deterministic latency. Component statistics (`ticks_processed_`, `ticks_dropped_`, etc.) are `std::atomic<uint64_t>` counters written by a single thread (`relaxed` ordering) and read by `MetricsCollector` from an external monitoring thread; because the counters are monotonically increasing and only sampled for reporting (not used to coordinate), `relaxed` is provably correct and avoids the fence overhead of stronger orderings. ThreadSanitizer analysis on the pipeline confirms zero data races in `mdp::` namespace code; any TSan reports on macOS native runs are confined to Apple system library internals (`libdispatch`, `_os_unfair_lock`) which are known false positives.

---

## Dependencies

All dependencies are fetched automatically via CMake `FetchContent` — no system packages or `brew install` required.

| Library | Version | Purpose | Fetched via |
|---|---|---|---|
| [GoogleTest](https://github.com/google/googletest) | v1.14.0 | Unit + integration testing framework | `FetchContent` |
| [Google Benchmark](https://github.com/google/benchmark) | v1.8.4 | Micro-benchmark framework with JSON output | `FetchContent` |
| [spdlog](https://github.com/gabime/spdlog) | v1.14.1 | Structured async logging (compiled lib mode) | `FetchContent` |
| [nlohmann/json](https://github.com/nlohmann/json) | v3.11.3 | JSON serialisation for `MetricsCollector` and health CLI | `FetchContent` |

---

## License

MIT License — see [LICENSE](LICENSE) for full text.

Copyright © 2026 Market Data Processor Project. All rights reserved.
