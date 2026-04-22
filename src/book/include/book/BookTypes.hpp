// Copyright (c) 2026 Market Data Processor Project. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace mdp {

/// @brief Side of an order in the book.
enum class OrderSide : uint8_t {
    BID = 0,  ///< A buy order.
    ASK = 1   ///< A sell order.
};

/// @brief Represents a single price level in the order book.
struct PriceLevel {
    double   price{0.0};
    uint64_t volume{0};

    bool operator==(const PriceLevel& other) const {
        return price == other.price && volume == other.volume;
    }

    bool operator<(const PriceLevel& other) const {
        return price < other.price;
    }
};

/// @brief Represents the best bid and ask prices for a given symbol.
struct TopOfBook {
    std::string_view symbol;
    double           best_bid{0.0};
    uint64_t         bid_volume{0};
    double           best_ask{0.0};
    uint64_t         ask_volume{0};
    double           spread{0.0};      // best_ask - best_bid
    uint64_t         timestamp_ns{0};

    /// @brief Checks if the TopOfBook data is valid.
    /// @return True if the book has a valid bid, ask, and spread.
    [[nodiscard]] bool is_valid() const noexcept {
        return best_bid > 0.0 && best_ask > 0.0 && best_ask > best_bid;
    }
};

/// @brief Represents a single price-level update derived from a MarketTick.
struct BookDelta {
    char      symbol[8]{};
    OrderSide side{OrderSide::BID};
    double    price{0.0};
    uint64_t  volume{0};  // 0 = remove this price level
    uint64_t  timestamp_ns{0};
};

/// @brief A full snapshot of an order book for a symbol.
struct BookSnapshot {
    char                    symbol[8]{};
    std::vector<PriceLevel> bids;  // sorted descending by price
    std::vector<PriceLevel> asks;  // sorted ascending by price
    uint64_t                timestamp_ns{0};
    uint64_t                sequence{0};  // monotonically increasing
};

}  // namespace mdp

