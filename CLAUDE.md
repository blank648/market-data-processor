# CLAUDE.md — Market Data Processor

> **Scop**: Fișier de context pentru Claude Code. Descrie proiectul, structura, convențiile de build și planul complet de testare în 5 sesiuni.

---

## Proiect

**Market Data Processor** este un pipeline C++20 de procesare a datelor de piață în timp real:

```
FeedSimulator ──► TickParser ──► Normalizer ──► BookProcessor ──► OrderBook
      │                                                                │
  (RingBuffer 16K)           (RingBuffer 4K)                    [BTCUSD / SOLUSD / ...]
```

**Stack tehnic**:
- Compilator: AppleClang 17 (macOS arm64)
- Standard: C++20
- Build: CMake + Ninja
- Teste: Google Test 1.14 + Google Mock
- Benchmarks: Google Benchmark 1.8.4
- Logging: spdlog 1.14.1
- JSON: nlohmann/json 3.11.3
- Sanitizare: ASan / UBSan / TSan

---

## Structura Directorului

```
market-data-processor/
├── src/
│   ├── feed/          ← FeedSimulator (producer SPSC)
│   ├── processing/    ← TickParser + Normalizer
│   ├── book/          ← OrderBook + BookProcessor
│   ├── strategy/
│   ├── infra/
│   └── core/
├── tests/
│   ├── CMakeLists.txt
│   ├── coretests.cpp
│   ├── feed_tests.cpp       ← NOU (generat Plan 1)
│   ├── processing_tests.cpp ← EXTINS (generat Plan 1)
│   ├── book_tests.cpp       ← EXTINS (generat Plan 1)
│   └── integration_test.cpp ← EXTINS (generat Plan 1)
├── benchmarks/
│   ├── CMakeLists.txt
│   └── benchmarks.cpp       ← NOU (generat Plan 1)
├── deps/              ← dependențe FetchContent (auto)
├── CMakeLists.txt
└── CLAUDE.md          ← acest fișier
```

---

## Comenzi Build

```bash
# Debug (implicit)
cmake -B build-debug -S .
cmake --build build-debug --parallel

# Release
cmake -B build-release -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --parallel

# ASan + UBSan
cmake -B build-asan -S . -DCMAKE_BUILD_TYPE=Debug -DENABLE_SANITIZERS=ON
cmake --build build-asan --parallel

# TSan (creat manual în Sesiunea 3)
cmake -B build-tsan -S . -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_FLAGS="-fsanitize=thread -g"
cmake --build build-tsan --parallel

# Coverage (creat manual în Sesiunea 5)
cmake -B build-cov -S . -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_FLAGS="--coverage -fprofile-arcs -ftest-coverage" \
      -DCMAKE_EXE_LINKER_FLAGS="--coverage"
cmake --build build-cov --parallel
```

---

## Rulare Teste

```bash
# Suite completă (debug)
cd build-debug && ctest --output-on-failure -V

# Filter per modul
./build-debug/tests/unittests --gtest_filter=FeedSimulatorTest.* -v
./build-debug/tests/unittests --gtest_filter=NormalizerTest.* -v
./build-debug/tests/unittests --gtest_filter=TickParserTest.* -v
./build-debug/tests/unittests --gtest_filter=OrderBookTest.* -v
./build-debug/tests/unittests --gtest_filter=BookProcessorTest.* -v
./build-debug/tests/unittests --gtest_filter=IntegrationTest.* -v

# Cu sanitizere
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 \
  ./build-asan/tests/unittests --gtest_output=xml:asan_results.xml

TSAN_OPTIONS="halt_on_error=0 history_size=7" \
  ./build-tsan/tests/unittests \
  --gtest_filter="NormalizerTest.*:IntegrationTest.*:FeedSimulatorTest.*" \
  --gtest_output=xml:tsan_results.xml

# Benchmarks
./build-release/benchmarks/mdp-benchmarks \
  --benchmark_format=console \
  --benchmark_out=benchmark_results.json \
  --benchmark_out_format=json
```

---

## Comportament Cunoscut (Nu Sunt Bug-uri)

| Log | Explicație |
|-----|-----------|
| `BOOK WARN Crossed book rejected for SOLUSD ask X best bid Y` | Comportament intenționat — ask < best_bid este respins; testul `CrossedBookInsertingAskLessThanBestBidIsRejected` validează asta |
| `ld: warning: ignoring duplicate libraries: .../libprocessing.a` | Duplicate link în `tests/CMakeLists.txt` — nu blochează build-ul; de corectat în Sesiunea 2 |
| `Process terminated due to timeout` (GTest discovery cu ASan) | ASan încetinește discovery-ul la 5s; nu este un eșec real de test |

---

## Convenții de Cod

- **SPSC RingBuffer**: niciodată 2 producători pe același buffer — UB garantat; în teste multi-thread se folosește mutex extern sau buffer dedicat per thread
- **`best_bid()` pe carte goală**: returnează `0.0` (sentinelă), nu `std::nullopt` — verificat în `OrderBook.hpp`; testele reflectă asta cu comentariu explicit
- **Thread lifecycle**: `start()` → `stop()` sunt idempotente; double-start și double-stop sunt no-op
- **Stats snapshot**: `stats()` returnează snapshot atomic — se poate citi după `stop()` fără tearing

---

## Plan de Testare — 5 Sesiuni Claude Code

> Execuți sesiunile în ordine. Fiecare sesiune are un scop clar și o condiție de finalizare.

---

### Sesiunea 1 — Build Health & Sanitizers

**Scop**: Verifică că build-ul este curat pe toate configurațiile și că testele existente trec.

**Condiție de finalizare**: `ctest` trece pe `build-debug`, nicio eroare ASan/UBSan pe `build-asan`.

```
Task: Verify build health for all three configurations and run existing tests.

1. Run:
   cmake -B build-debug -S . -DCMAKE_BUILD_TYPE=Debug
   cmake --build build-debug --parallel
   cd build-debug && ctest --output-on-failure -V

2. Run:
   cmake -B build-asan -S . -DCMAKE_BUILD_TYPE=Debug -DENABLE_SANITIZERS=ON
   cmake --build build-asan --parallel
   ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 \
     ./build-asan/tests/unittests --gtest_output=xml:asan_results.xml

3. Report:
   - Any ASAN/UBSAN violations with exact stack trace
   - Any test failures with full GTEST output
   - The "ld: warning: ignoring duplicate libraries" for libprocessing.a:
     fix the duplicate link in tests/CMakeLists.txt

Fix any issues found before proceeding.
```

---

### Sesiunea 2 — Integrare Fișiere Noi + Fix Compile Errors

**Scop**: Adaugă fișierele generate de Antigravity în build și fixează orice erori de compilare.

**Condiție de finalizare**: Toate cele 5 module de teste compilează și trec pe `build-debug`.

```
Task: Add the new test files generated by Antigravity into the build.

Files to add:
- tests/feed_tests.cpp
- Updated tests/processing_tests.cpp
- Updated tests/book_tests.cpp
- Updated tests/integration_test.cpp
- benchmarks/benchmarks.cpp

Steps:
1. Verify tests/CMakeLists.txt includes feed_tests.cpp in the unittests target sources
2. cmake --build build-debug --parallel
3. If any compilation errors: fix them immediately in-place (do not regenerate)
4. cmake --build build-asan --parallel
5. Fix any additional ASAN-specific compilation issues
6. Run each test suite individually:
   ./build-debug/tests/unittests --gtest_filter=FeedSimulatorTest.* -v
   ./build-debug/tests/unittests --gtest_filter=NormalizerTest.* -v
   ./build-debug/tests/unittests --gtest_filter=OrderBookTest.* -v
   ./build-debug/tests/unittests --gtest_filter=TickParserTest.* -v
   ./build-debug/tests/unittests --gtest_filter=BookProcessorTest.* -v
7. Report all FAILED tests with full GTest output

NOTE: OrderBookTest.EmptyBookBestBidAndAskReturnsZero checks for 0.0, not
std::nullopt — this is intentional per OrderBook.hpp implementation.
Add a comment in book_tests.cpp confirming this is expected behavior if not
already present.
```

---

### Sesiunea 3 — Thread Sanitizer (TSan)

**Scop**: Detectează race conditions rămase în codul concurent.

**Condiție de finalizare**: Zero TSan DATA RACE raportate pe `NormalizerTest.*:IntegrationTest.*`.

```
Task: Run ThreadSanitizer to catch remaining concurrency bugs.

1. cmake -B build-tsan -S . -DCMAKE_BUILD_TYPE=Debug \
         -DCMAKE_CXX_FLAGS="-fsanitize=thread -g"
   cmake --build build-tsan --parallel

2. TSAN_OPTIONS="halt_on_error=0 history_size=7" \
     ./build-tsan/tests/unittests \
     --gtest_filter="NormalizerTest.*:IntegrationTest.*:FeedSimulatorTest.*" \
     --gtest_output=xml:tsan_results.xml

3. For every TSan DATA RACE reported:
   a. Show the exact race (which threads, which addresses)
   b. Fix it (atomic, mutex, or restructure)
   c. Re-run the specific test to confirm clean

4. Run: ./build-tsan/tests/unittests --gtest_filter=IntegrationTest.* -v
   Confirm GracefulShutdownUnderLoad completes within timeout.

NOTE: NormalizerTest.ConcurrentSafetyWith10000Ticks uses an external mutex
to serialize access to the SPSC RingBuffer — this is intentional and not a
TSan issue. TSan may flag it as unnecessary synchronization; that is acceptable.
```

---

### Sesiunea 4 — Integration & Runtime Validation

**Scop**: Validare completă a runtime-ului, output spdlog și benchmarks.

**Condiție de finalizare**: Full suite trece, benchmark_results.json generat, throughput raportat.

```
Task: Full integration run + manual output validation.

1. Run full suite:
   ./build-debug/tests/unittests --gtest_output=xml:full_results.xml -v

2. Parse full_results.xml and summarize:
   - Total tests / PASSED / FAILED / SKIPPED
   - Any test taking > 1000ms (flag as potential deadlock risk)

3. Run integration test with spdlog output visible:
   SPDLOG_LEVEL=debug ./build-debug/tests/unittests \
     --gtest_filter=IntegrationTest.* -v 2>&1 | head -200

4. Verify in the output:
   a. "Crossed book rejected" lines appear (EXPECTED — do NOT suppress)
   b. No "thread still running" or "join timeout" messages
   c. No spdlog CRITICAL or ERROR lines (only WARN for crossed books is acceptable)

5. Run benchmarks:
   cmake --build build-release --parallel
   ./build-release/benchmarks/mdp-benchmarks \
     --benchmark_format=console \
     --benchmark_out=benchmark_results.json \
     --benchmark_out_format=json

6. From benchmark_results.json extract and report:
   - BM_FullPipeline_Throughput: ticks/sec
     NOTE: FeedSimulator runs idle in this benchmark; main thread is the producer.
     Result reflects ingestie latency, not full pipeline throughput.
   - BM_RingBuffer_PushPop: latency in ns
   - Flag any benchmark with CV > 5% as unstable
```

---

### Sesiunea 5 — Coverage Report (Gate înainte de Dashboard)

**Scop**: Generează raport de acoperire și validează că proiectul e gata pentru integrare.

**Condiție de finalizare**: Coverage global >= 75%; raport HTML generat în `coverage_html/`.

```
Task: Generate line coverage report as integration gate.

1. cmake -B build-cov -S . -DCMAKE_BUILD_TYPE=Debug \
         -DCMAKE_CXX_FLAGS="--coverage -fprofile-arcs -ftest-coverage" \
         -DCMAKE_EXE_LINKER_FLAGS="--coverage"
   cmake --build build-cov --parallel

2. ./build-cov/tests/unittests

3. lcov --capture --directory build-cov \
        --output-file coverage.info \
        --ignore-errors mismatch
   lcov --remove coverage.info '/usr/*' '*/deps/*' \
        --output-file coverage_filtered.info
   genhtml coverage_filtered.info --output-directory coverage_html

4. Report:
   - Overall line coverage %
   - Coverage per module: feed / processing / book / strategy / infra
   - Any file below 70% coverage → list specific uncovered lines

5. GATE: If overall coverage < 75%, report which tests are missing
   and suggest specific test cases to add before Dashboard integration.

6. If coverage >= 75% AND TSan is clean (Sesiunea 3 passed):
   Print: "✅ Market Data Processor is ready for Market Dashboard integration."
   Else:
   Print: "❌ Blockers remaining: [list issues]"
```

---

## Istoricul Build-urilor (Referință)

- `build-debug`: configurat cu AppleClang 17, compils cu warnings despre `libprocessing.a` duplicate
- `build-release`: compils curat, `unittests` linkuit cu succes
- `build-asan`: compils curat; `GTestAddTests` timeout la discovery cu ASan — nu este eșec real
- `build-tsan`: de creat în Sesiunea 3

---

## Output Așteptat Final

```
[✅] Sesiunea 1: build-debug + build-asan curate
[✅] Sesiunea 2: toate testele compilează și trec (feed/processing/book/integration)
[✅] Sesiunea 3: zero TSan DATA RACE
[✅] Sesiunea 4: full suite PASSED, benchmark_results.json generat
[✅] Sesiunea 5: coverage >= 75% → ready for Market Dashboard integration
```
