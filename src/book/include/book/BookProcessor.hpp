// Copyright (c) 2026 Market Data Processor Project. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <atomic>
#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>

#include "BookTypes.hpp"
#include "OrderBook.hpp"
#include "core/MarketSnapshot.hpp"
#include "core/MarketTick.hpp"
#include "core/RingBuffer.hpp"
#include "core/ThreadBase.hpp"

namespace mdp {

// Type alias for clarity
using TickRingBuffer4K = RingBuffer<MarketTick, 4096>;
using SnapshotRingBuffer4K = RingBuffer<MarketSnapshot, 4096>;

/// @brief A pipeline stage that consumes normalized ticks and builds order books.
class BookProcessor final : public ThreadBase<BookProcessor> {
    friend class ThreadBase<BookProcessor>;

   public:
    /// @brief Constructs the processor, linking it to an input ring buffer.
    /// @param input The ring buffer containing normalized MarketTick data.
    explicit BookProcessor(TickRingBuffer4K& input);
    BookProcessor(TickRingBuffer4K& input, SnapshotRingBuffer4K& db_queue);
    BookProcessor(TickRingBuffer4K& input, SnapshotRingBuffer4K& db_queue,
                  SnapshotRingBuffer4K& signal_queue);

    /// @brief Destructor. Stops the worker thread.
    ~BookProcessor() override {
        stop();
    }

    // Non-copyable, non-movable
    BookProcessor(const BookProcessor&) = delete;
    BookProcessor& operator=(const BookProcessor&) = delete;
    BookProcessor(BookProcessor&&) = delete;
    BookProcessor& operator=(BookProcessor&&) = delete;

    /// @brief Access the order book for a given symbol.
    /// @param symbol The symbol to look up.
    /// @return A const pointer to the OrderBook, or nullptr if the symbol has not been seen yet.
    /// @note [NOT THREAD-SAFE] Intended for post-stop() access only; calling
    /// book() while run() is active is a data race.
    [[nodiscard]] const OrderBook* book(std::string_view symbol) const noexcept;

    /// @brief Gets the total number of ticks processed from the input buffer.
    /// @return The number of processed ticks.
    [[nodiscard]] uint64_t ticks_processed() const noexcept;

    /// @brief Gets the number of unique symbols being tracked (active books).
    /// @return The number of active order books.
    [[nodiscard]] uint64_t books_active() const noexcept;

    /// @brief Gets the total number of snapshots dropped because the output ring buffer was full.
    /// @return The count of dropped snapshots.
    [[nodiscard]] uint64_t snapshots_dropped() const noexcept;

   protected:
    /// @brief The main worker loop for the thread.
    /// @param st A stop token for cooperative cancellation.
    void run(StopToken stop_token);

   private:
    TickRingBuffer4K& input_;
    SnapshotRingBuffer4K* db_queue_{nullptr};
    SnapshotRingBuffer4K* signal_queue_{nullptr};
    std::unordered_map<std::string, OrderBook> books_;
    std::atomic<uint64_t> ticks_processed_{0};
    std::atomic<uint64_t> snapshots_dropped_{0};

    // Side determination: price < reference -> BID, price >= reference -> ASK
    // Reference price per symbol = exponential moving average of price
    std::unordered_map<std::string, double> reference_price_;

    /// @brief Determines the side of a tick based on its price relative to a reference.
    /// @param symbol The tick's symbol.
    /// @param price The tick's price.
    /// @return The determined OrderSide (BID or ASK).
    [[nodiscard]] OrderSide determine_side(std::string_view symbol, double price) noexcept;

    /// @brief Updates the exponential moving average reference price for a symbol.
    /// @param symbol The symbol to update.
    /// @param price The new price to incorporate.
    void update_reference(std::string_view symbol, double price) noexcept;

    /// @brief Processes a single market tick.
    /// @param tick The tick to process.
    void process_tick(const MarketTick& tick);

    /// @brief Converts a MarketTick to a BookDelta.
    /// @param tick The source tick.
    /// @param side The determined side for the tick.
    /// @return A BookDelta representing the price level update.
    static BookDelta tick_to_delta(const MarketTick& tick, OrderSide side) noexcept;
};

}  // namespace mdp
