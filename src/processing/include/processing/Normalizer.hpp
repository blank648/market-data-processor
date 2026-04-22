// Copyright (c) 2026 Market Data Processor Project. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <cstdint>
#include <unordered_map>

#include "core/MarketTick.hpp"
#include "core/RingBuffer.hpp"
#include "core/ThreadBase.hpp"

namespace mdp {

/// @brief Statistics for the Normalizer processing stage.
// Atomic fields prevent data race between run() (writer, jthread)
// and stats() (reader, any thread). memory_order_relaxed is
// sufficient for monotonic counters read post-stop().
struct NormalizerStats {
    /// Total number of clean ticks successfully forwarded downstream.
    std::atomic<uint64_t> ticks_forwarded{0};

    /// Number of exact duplicate ticks (same symbol, price, and timestamp_ns) dropped.
    std::atomic<uint64_t> ticks_deduplicated{0};

    /// Number of ticks dropped because their timestamp went backward relative to previous ticks.
    std::atomic<uint64_t> ticks_reordered{0};

    // Snapshot for external readers (test thread, metrics)
    // Returns plain struct copy — safe to read post-stop() or
    // from any thread (atomics guarantee no tearing).
    struct Snapshot {
        uint64_t ticks_forwarded{0};
        uint64_t ticks_deduplicated{0};
        uint64_t ticks_reordered{0};
    };
    [[nodiscard]] Snapshot snapshot() const noexcept {
        return {ticks_forwarded.load(std::memory_order_relaxed),
                ticks_deduplicated.load(std::memory_order_relaxed),
                ticks_reordered.load(std::memory_order_relaxed)};
    }
};

/// @brief Deduplicates and monotonically orders market data ticks per symbol.
///
/// Normalizer is pipeline Stage 3. It reads validated ticks from TickParser,
/// applies deduplication and monotonic timestamp checking per symbol, and
/// forwards clean ticks to the next stage buffer.
class Normalizer final : public ThreadBase {
   public:
    /// @brief Constructs a Normalizer connecting input and output ring buffers.
    ///
    /// @param input  Buffer to read validated ticks from.
    /// @param output Buffer to write normalized ticks to.
    explicit Normalizer(TickRingBuffer4K& input, TickRingBuffer4K& output);

    ~Normalizer() override {
        stop();
    }

    /// @brief Returns a thread-safe snapshot of the current statistics.
    ///
    /// @return A copy of the current NormalizerStats::Snapshot.
    [[nodiscard]] NormalizerStats::Snapshot stats() const noexcept;

   private:
    /// @brief Main processing loop for the normalizer thread.
    ///
    /// @param st Token to check for cooperative cancellation.
    void run(StopToken st) override;

    /// @brief Checks if a tick is an exact duplicate of the last processed tick for its symbol.
    ///
    /// @param tick The tick to check.
    /// @return true if it's a duplicate; false otherwise.
    [[nodiscard]] bool is_duplicate(const MarketTick& tick) const noexcept;

    /// @brief Checks if a tick's timestamp is strictly less than the last processed timestamp for
    /// its symbol.
    ///
    /// @param tick The tick to check.
    /// @return true if reordered; false otherwise.
    [[nodiscard]] bool is_reordered(const MarketTick& tick) const noexcept;

    /// @brief Updates the internal state for the tick's symbol.
    ///
    /// @param tick The newly processed tick.
    void update_state(const MarketTick& tick) noexcept;

    TickRingBuffer4K& input_;
    TickRingBuffer4K& output_;

    /// @brief Custom hash functor for a fixed-size char array representing a symbol.
    struct SymbolHash {
        std::size_t operator()(const std::array<char, 8>& s) const noexcept;
    };

    // [DEDUP STRATEGY]
    // We maintain per-symbol state using unordered_map to track the last seen
    // timestamp and the full last tick. We use std::array<char, 8> for keys
    // instead of std::string_view since string views would dangerously point
    // to memory inside temporary MarketTick structs that get overwritten.

    // [NOT THREAD-SAFE] Accessed exclusively from run() (jthread).
    // stats() does not touch these maps — safe today.
    // Any future cross-thread access requires a mutex guard.
    std::unordered_map<std::array<char, 8>, int64_t, SymbolHash> last_timestamp_;

    // [NOT THREAD-SAFE] Same contract as last_timestamp_.
    // Future extension: if read from metrics/dashboard thread,
    // protect both maps with a std::shared_mutex (read-heavy access).
    std::unordered_map<std::array<char, 8>, MarketTick, SymbolHash> last_tick_;

    NormalizerStats stats_;
};

}  // namespace mdp
