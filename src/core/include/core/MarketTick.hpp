// Copyright (c) 2026 Market Data Processor Project. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <format>
#include <iosfwd>
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <string_view>

namespace mdp {

/// @brief Represents a single market data tick.
///
/// MarketTick is a trivially copyable, fixed-size (40-byte) POD-like structure
/// suitable for lock-free ring buffers, memory-mapped I/O, and low-latency
/// pipelines. All fields are laid out with explicit padding to ensure a
/// predictable, ABI-stable memory footprint across platforms.
///
/// Example usage:
/// @code
///     auto tick = mdp::MarketTick::make("BTCUSD", 42000.50, 1.23, 1);
///     std::cout << tick << '\n';
/// @endcode
struct MarketTick {
    /// Fixed-length symbol field (8 bytes). Unused trailing bytes are zero-filled.
    /// Example: "BTCUSD\0\0"
    std::array<char, 8> symbol;

    /// Mid-price of the instrument at the time of the tick.
    double price;

    /// Trade or quote volume associated with this tick.
    double volume;

    /// Nanoseconds since the steady clock epoch at the time the tick was captured.
    int64_t timestamp_ns;

    /// Side indicator: 0 = bid, 1 = ask, 2 = trade.
    std::uint8_t side;

    /// Explicit padding to bring total struct size to 40 bytes.
    std::uint8_t _pad[7];  // NOLINT(readability-identifier-naming)

    // ─── Factory ────────────────────────────────────────────────────────────

    /// @brief Constructs a MarketTick from the provided market data fields.
    ///
    /// Copies up to 8 characters from @p sym into the symbol array (zero-fills
    /// any remaining bytes). Captures the current timestamp from
    /// `std::chrono::steady_clock`.
    ///
    /// @param sym   Instrument symbol (truncated to 8 characters if longer).
    /// @param price Mid-price of the instrument.
    /// @param volume Trade or quote volume.
    /// @param side  Side indicator: 0=bid, 1=ask, 2=trade.
    /// @return A fully initialised MarketTick.
    [[nodiscard]] static MarketTick make(std::string_view sym, double price, double volume,
                                         std::uint8_t side) noexcept {
        MarketTick t{};
        // Copy symbol, truncating at 8 chars; remaining bytes already zero.
        const std::size_t len = sym.size() < 8u ? sym.size() : 8u;
        for (std::size_t i = 0; i < len; ++i) {
            t.symbol[i] = sym[i];
        }
        t.price = price;
        t.volume = volume;
        t.side = side;
        t.timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count();
        return t;
    }

    // ─── Formatting ─────────────────────────────────────────────────────────

    /// @brief Returns a human-readable representation of this tick.
    ///
    /// Format:
    /// @code
    ///     "BTCUSD | price=42000.50 | vol=1.2300 | ts=123456789ns | side=BID"
    /// @endcode
    ///
    /// The `side` field is rendered as "BID", "ASK", or "TRADE".
    ///
    /// @return A formatted string describing this tick.
    [[nodiscard]] std::string to_string() const {
        // Build a null-terminated view of the symbol field.
        const std::string_view sym_view{symbol.data(), [this]() -> std::size_t {
                                            for (std::size_t i = 0; i < symbol.size(); ++i) {
                                                if (symbol[i] == '\0')
                                                    return i;
                                            }
                                            return symbol.size();
                                        }()};

        const std::string_view side_str = [this]() -> std::string_view {
            switch (side) {
                case 0:
                    return "BID";
                case 1:
                    return "ASK";
                default:
                    return "TRADE";
            }
        }();

        return std::format("{} | price={:.2f} | vol={:.4f} | ts={}ns | side={}", sym_view, price,
                           volume, timestamp_ns, side_str);
    }

    // ─── Operators ──────────────────────────────────────────────────────────

    /// @brief Equality comparison using all data members.
    ///
    /// Compiler-generated member-wise comparison; `_pad` bytes are included,
    /// so callers should ensure padding is zero-initialised (as `make()` does).
    bool operator==(const MarketTick&) const noexcept = default;

    /// @brief Streams the tick to @p os via `to_string()`.
    ///
    /// @param os Output stream to write to.
    /// @param t  The tick to format.
    /// @return Reference to @p os for chaining.
    friend std::ostream& operator<<(std::ostream& os, const MarketTick& t) {
        return os << t.to_string();
    }

    // ─── JSON (forward-declared) ─────────────────────────────────────────────

    /// @brief Serialises this tick into a nlohmann::json object.
    ///
    /// The full definition lives in a separate translation unit that includes
    /// `<nlohmann/json.hpp>` to keep compile times low.
    ///
    /// @param j JSON object to populate.
    void to_json(nlohmann::json& j) const;
};

// ─── Static Assertions ────────────────────────────────────────────────────────

static_assert(std::is_trivially_copyable_v<MarketTick>,
              "MarketTick must be trivially copyable for use in lock-free structures.");

static_assert(sizeof(MarketTick) == 40,
              "MarketTick must be exactly 40 bytes; check field layout and padding.");

}  // namespace mdp
