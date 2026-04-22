// Copyright (c) 2026 Market Data Processor Project. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include "IOrderBook.hpp"
#include "BookTypes.hpp"
#include <cstdint>
#include <functional>
#include <map>
#include <string_view>

namespace mdp {

/// @brief A concrete, single-symbol, non-thread-safe order book implementation.
class OrderBook final : public IOrderBook {
public:
    /// @brief Constructs an order book for a specific symbol.
    /// @param symbol The financial instrument's symbol.
    explicit OrderBook(std::string_view symbol);

    /// @brief Apply a delta update (a single price level change).
    /// @param delta The change to apply to the book.
    void apply(const BookDelta& delta) noexcept override;

    /// @brief Return the current top-of-book state.
    /// @return A TopOfBook struct representing the best bid and ask.
    [[nodiscard]] TopOfBook top_of_book() const noexcept override;

    /// @brief Return a full snapshot of all price levels.
    /// @return A BookSnapshot containing all bid and ask levels.
    [[nodiscard]] BookSnapshot snapshot() const override;

    /// @brief Get the number of price levels on the bid side.
    /// @return The count of bid levels.
    [[nodiscard]] std::size_t bid_levels() const noexcept override;

    /// @brief Get the number of price levels on the ask side.
    /// @return The count of ask levels.
    [[nodiscard]] std::size_t ask_levels() const noexcept override;

    /// @brief Clear all price levels from the book.
    void clear() noexcept override;

    /// @brief Get the highest bid price.
    [[nodiscard]] double best_bid() const noexcept {
        return bids_.empty() ? 0.0 : bids_.rbegin()->first;
    }

    /// @brief Get the lowest ask price.
    [[nodiscard]] double best_ask() const noexcept {
        return asks_.empty() ? 0.0 : asks_.begin()->first;
    }

    /// @brief Gets the total number of updates applied to this book.
    /// @return The number of successful calls to apply().
    [[nodiscard]] uint64_t updates_applied() const noexcept;

    /// @brief Gets the total number of price levels removed (volume set to 0).
    /// @return The number of removed levels.
    [[nodiscard]] uint64_t levels_removed() const noexcept;

    /// @brief Gets the symbol this book is tracking.
    /// @return The symbol as a string_view.
    [[nodiscard]] std::string_view symbol() const noexcept;

private:
    char symbol_[8]{};
    // [CV NOTE] std::map is used for its O(log N) update performance and
    // iterator stability. For the expected low number of price levels per
    // symbol and sub-microsecond tick rates, this is an acceptable trade-off
    // compared to a `boost::container::flat_map` or sorted `std::vector`,
    // which would offer faster lookups but O(N) updates. Iterator stability
    // would be a benefit if `snapshot()` were called concurrently with
    // `apply()`, but this class is not designed for concurrent access.

    /// @brief Bids sorted from lowest price to highest (default map order).
    std::map<double, uint64_t> bids_;
    /// @brief Asks sorted from lowest price to highest.
    std::map<double, uint64_t> asks_;

    uint64_t sequence_{0};
    uint64_t updates_applied_{0};
    uint64_t levels_removed_{0};
};

}  // namespace mdp


