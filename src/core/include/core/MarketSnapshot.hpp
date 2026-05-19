// Copyright (c) 2026 Market Data Processor Project. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <cstdint>

namespace mdp {

/// @brief Represents a consolidated snapshot of the top of the book for a symbol.
///
/// MarketSnapshot satisfies the `TickLike` concept because it provides
/// `price` and `timestamp_ns` fields.
struct MarketSnapshot {
    std::array<char, 8> symbol{};
    double              price{0.0};        // e.g. mid-price
    double              volume{0.0};       // e.g. accumulated volume
    int64_t             timestamp_ns{0};
    int64_t             sequence{0};
};

}  // namespace mdp
