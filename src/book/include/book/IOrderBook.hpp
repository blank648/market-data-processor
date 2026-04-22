// Copyright (c) 2026 Market Data Processor Project. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>

namespace mdp {

// Forward declarations
struct BookDelta;
struct TopOfBook;
struct BookSnapshot;

/// @brief Pure interface for a single-symbol order book.
/// @note This interface is designed to be used by value or reference, not polymorphic pointers.
/// No virtual destructor is provided.
class IOrderBook {
public:
    /// @brief Apply a delta update (a single price level change).
    /// @param delta The change to apply to the book.
    virtual void apply(const BookDelta& delta) noexcept = 0;

    /// @brief Return the current top-of-book state.
    /// @return A TopOfBook struct representing the best bid and ask.
    [[nodiscard]] virtual TopOfBook top_of_book() const noexcept = 0;

    /// @brief Return a full snapshot of all price levels.
    /// @return A BookSnapshot containing all bid and ask levels.
    [[nodiscard]] virtual BookSnapshot snapshot() const = 0;

    /// @brief Get the number of price levels on the bid side.
    /// @return The count of bid levels.
    [[nodiscard]] virtual std::size_t bid_levels() const noexcept = 0;

    /// @brief Get the number of price levels on the ask side.
    /// @return The count of ask levels.
    [[nodiscard]] virtual std::size_t ask_levels() const noexcept = 0;

    /// @brief Clear all price levels from the book. Used in tests.
    virtual void clear() noexcept = 0;
};

}  // namespace mdp

