***

## Viziunea Proiectului

Scopul este să construiești un **pipeline de procesare a datelor financiare în timp real**, cu accent pe latență scăzută, lock-free concurrency și design modular. Nu simulăm un exchange real — simulăm *feed-ul* (tick data) și demonstrăm că știi să gestionezi throughput ridicat fără race conditions sau bottlenecks.

***

## Decizia Arhitecturală: Pipeline + SPSC Ring Buffer

Arhitectura aleasă este un **multi-stage pipeline** unde fiecare stage rulează pe propriul thread și comunică prin **SPSC (Single Producer Single Consumer) lock-free ring buffers**.  Aceasta este exact abordarea LMAX Disruptor, care elimină contention-ul pe shared queue și maximizează cache locality.

```
[ FeedSimulator ] ──SPSC──▶ [ Parser ] ──SPSC──▶ [ Normalizer ]
                                                        │
                                              ┌─────────┴─────────┐
                                       [ OrderBook ]       [ SignalEngine ]
                                              │                    │
                                       [ AsyncLogger ] ◀──────────┘
                                              │
                                       [ MetricsCollector ]
```

**De ce NU mutex-based queue?** Mutex-urile implică kernel syscalls, context switches și cache invalidation.  Pe hot path, un SPSC atomic ring buffer cu `acquire/release` memory ordering este de 5-10x mai rapid.

***

## Stack Tehnologic

| Componentă | Alegere | Motivație |
| :-- | :-- | :-- |
| Standard | **C++20** | `std::jthread`, `std::stop_token`, Concepts |
| Build | **CMake 3.28 + Ninja** | FetchContent pentru deps, Ninja rapid |
| Testing | **Google Test** | Industry standard, integrare CLion nativă |
| Benchmarks | **Google Benchmark** | Latency microsecond-level [^1] |
| Logging | **spdlog** (async sink) | Header-only, async, zero-alloc hot path |
| Data format | **nlohmann/json** | Feed simulation în JSON → binary intern |
| Deps | **CPM.cmake / FetchContent** | No submodules, clean repo |


***

## Structura Modulelor (6 module clare)

### `core/` — Tipuri de date și infrastructură

- `MarketTick.hpp` — struct POD cu `symbol`, `price (double)`, `volume`, `timestamp_ns` (nanosecunde)
- `RingBuffer<T, N>` — **template SPSC lock-free**, `alignas(64)` pentru cache line isolation, capacitate power-of-2
- `ThreadBase.hpp` — RAII wrapper cu `std::jthread` + `std::stop_token`


### `feed/` — Simulatorul de date

- `FeedSimulator` — generează tick-uri sintetice (random walk pe preț + volume), publică în ring buffer la ~1M msg/sec configurabil


### `processing/` — Parsing + Normalizare

- `TickParser` — deserializează raw bytes → `MarketTick` (zero-copy unde posibil)
- `Normalizer` — deduplicare, timestamp monotonic check, symbol mapping


### `book/` — Order Book

- `OrderBook` — `std::map<double, uint64_t>` pentru bid/ask price levels, top-of-book spread calculat la fiecare update[^3]


### `strategy/` — Semnale

- `SignalEngine` — EMA (Exponential Moving Average) și VWAP (Volume-Weighted Average Price) pe sliding window, emit `Signal` events


### `infra/` — Observabilitate

- `AsyncLogger` — spdlog async sink, **nu blochează hot path niciodată**
- `LatencyTracker` — `std::atomic<uint64_t>` histogramă de latență end-to-end

***

## C++20 Features Showcase (important pentru CV)

Acestea sunt exact conceptele pe care recrutorii le caută și trebuie să fie vizibile în cod:[^2][^1]

- **`std::jthread` + `std::stop_token`** — lifecycle management curat fără `volatile bool running`
- **`std::atomic<T>` cu memory ordering explicit** — `memory_order_acquire` / `memory_order_release` în SPSC queue (nu doar `seq_cst` naive)
- **`alignas(64)`** — false sharing prevention între producer/consumer counters
- **Concepts** — `template<TickLike T>` constraint pe RingBuffer
- **`constexpr`** — validare power-of-2 la compile time pentru buffer size
- **Move semantics** — zero-copy transfer al tick-urilor prin pipeline

***

## Sprint Plan (3 săptămâni)

### Sprint 0 — Setup (Ziua 1-2)

*Tool: Gemini CLI*

- Structura de directoare, `CMakeLists.txt` root + per-module
- FetchContent pentru GTest, Google Benchmark, spdlog, nlohmann/json
- `.clang-format`, `.clang-tidy`, `.gitignore`
- GitHub Actions CI: build + test pe macOS


### Sprint 1 — Core Primitives (Ziua 3-6)

*Tool: Antigravity → schelet, Copilot → completare*

- `MarketTick` struct + serialization
- `RingBuffer<T,N>` SPSC — **cel mai important modul, scrie-l manual**
- `ThreadBase` RAII wrapper
- Unit tests pentru RingBuffer (producere/consum, wrap-around, full/empty)


### Sprint 2 — Feed + Parser (Ziua 7-9)

*Tool: Copilot daily coding*

- `FeedSimulator` cu configurabil tick rate
- `TickParser` + `Normalizer`
- Integration test: Feed → Parser → assert tick integrity


### Sprint 3 — Order Book (Ziua 10-13)

*Tool: Gemini Code Assist pentru review*

- `OrderBook` cu bid/ask maps
- Top-of-book spread tracking
- Snapshot + delta update support
- Unit tests full coverage


### Sprint 4 — Signal Engine (Ziua 14-16)

*Tool: Copilot + Perplexity pentru formule*

- EMA cu configurable period (e.g. EMA-20)
- VWAP pe rolling window
- `Signal` struct emis downstream


### Sprint 5 — Infra + Observabilitate (Ziua 17-18)

*Tool: Gemini CLI pentru automatizare*

- `AsyncLogger` integrat în pipeline
- `LatencyTracker` — măsoară ns de la FeedSimulator → SignalEngine
- Google Benchmark suite: RingBuffer throughput, OrderBook update latency


### Sprint 6 — Polish + CV-Ready (Ziua 19-21)

*Tool: Gemini Code Assist pentru refactoring final*

- README.md profesional cu arhitectură diagram + benchmark results
- Doxygen comments pe interfețele publice
- Profiling cu Instruments (macOS) sau `perf`
- `BENCHMARKS.md` cu rezultate reale (numbers matter pe CV!)

***

## Decizii Tehnice Cheie

**1. SPSC vs MPMC?** — Folosești SPSC între fiecare pereche de stage-uri adiacente. Dacă vrei să extinzi cu multi-source feeds, adaugi un MPSC queue doar la intrarea în Parser.[^4]

**2. Bounded buffer size?** — Da, **întotdeauna bounded** (power-of-2, ex: 4096 slots). Unbounded queues pot epuiza memoria dacă consumatorul rămâne în urmă.[^1]

**3. double vs int64 pentru prețuri?** — În producție se folosesc `int64` (price în bps/ticks). Pe CV este OK `double`, dar menționează în README că ești conștient de alternativă — arată maturitate.

**4. Simulare vs API real?** — Simulare pentru MVP, dar adaugă un `IFeedSource` interface abstract. Astfel poți conecta ulterior Alpaca/Binance WebSocket API ca extensie opțională — demonstrează gândire extensibilă.[^3]

***

## Metrici Țintă pentru README

Acestea sunt cifrele care impresionează pe CV — trebuie să le măsori și să le publici:

- RingBuffer throughput: **> 10M ops/sec** (single-threaded benchmark)
- End-to-end latency Feed→Signal: **< 1 µs** median pe Apple Silicon
- OrderBook update latency: **< 500 ns** per tick

***

## Workflow cu Tool-urile Tale

| Fază | Tool | Cum îl folosești |
| :-- | :-- | :-- |
| Design / decizie | **Perplexity** | "Ce memory ordering folosesc pentru SPSC publish?" |
| Schelet modul nou | **Antigravity** | "Generează scheletul clasei RingBuffer<T,N>" |
| Cod zilnic | **Copilot Pro** | Completare inline în CLion, flow state |
| Review / refactor | **Gemini Code Assist** | "Explică de ce am false sharing aici", "Refactorizează ThreadBase" |
| Build / test / git | **Gemini CLI** | `cmake --build`, `ctest`, `git commit -m "feat: add SPSC ring buffer"` |


