// Copyright (c) 2026 Market Data Processor Project. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <atomic>
#include <cstdint>

#include "core/MarketTick.hpp"
#include "core/RingBuffer.hpp"
#include "core/ThreadBase.hpp"

namespace mdp {

/// @brief Validates, enriches, and forwards market data ticks.
///
/// TickParser is pipeline Stage 2. It reads raw MarketTick entries from the
/// FeedSimulator output buffer, validates them (e.g., checks for positive
/// prices and valid timestamps), enriches them (e.g., normalizes side
/// indicators and ensures string null-termination), and writes the
/// validated ticks to the next stage buffer.
class TickParser final : public ThreadBase {
   public:
    /// @brief Constructs a TickParser connecting input and output ring buffers.
    ///
    /// @param input  Buffer to read raw ticks from.
    /// @param output Buffer to write validated and enriched ticks to.
    explicit TickParser(TickRingBuffer16K& input, TickRingBuffer4K& output);

    ~TickParser() override {
        stop();
    }

    /// @brief Returns the total number of ticks successfully processed and forwarded.
    ///
    /// @return The count of processed ticks.
    [[nodiscard]] uint64_t ticks_processed() const noexcept;

    /// @brief Returns the total number of ticks rejected due to validation failures.
    ///
    /// @return The count of rejected ticks.
    [[nodiscard]] uint64_t ticks_rejected() const noexcept;

   private:
    /// @brief Main processing loop for the parser thread.
    ///
    /// Continuously reads from the input buffer, validates and enriches ticks,
    /// and blocks with back-pressure if the output buffer is full.
    ///
    /// @param st Token to check for cooperative cancellation.
    void run(StopToken st) override;

    /// @brief Validates a market tick against predefined rules.
    ///
    /// A tick is rejected if price is <= 0 or invalid, volume <= 0 or invalid,
    /// timestamp_ns <= 0, side > 2, or symbol is empty.
    ///
    /// @param tick The tick to validate.
    /// @return true if the tick is valid; false if it should be discarded.
    [[nodiscard]] bool validate(const MarketTick& tick) const noexcept;

    /// @brief Enriches and normalizes a valid market tick.
    ///
    /// Ensures null-termination of the symbol string and clamps the side
    /// field to a maximum of 2 (trade).
    ///
    /// @param tick The tick to enrich (modified in-place).
    void enrich(MarketTick& tick) const noexcept;

    TickRingBuffer16K& input_;
    TickRingBuffer4K& output_;
    std::atomic<uint64_t> ticks_processed_{0};
    std::atomic<uint64_t> ticks_rejected_{0};
};

}  // namespace mdp
