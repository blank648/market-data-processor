#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mdp {

/// @brief Configuration structure for the feed simulation pipeline.
///
/// Plain data structure defining the properties of a market data feed simulator,
/// including symbol lists, rates, and buffer setup.
struct FeedConfig {
    /// @brief List of symbols to simulate (e.g., {"BTCUSD", "ETHUSD", "SOLUSD"}).
    std::vector<std::string> symbols;

    /// @brief Initial prices parallel to the symbols list.
    std::vector<double> initial_prices;

    /// @brief Target number of ticks generated per second (e.g., 100'000).
    uint64_t tick_rate_hz;

    /// @brief Standard deviation of price changes per tick (e.g., 0.001).
    double volatility;

    /// @brief Size of the ring buffer. Must be a power of two.
    std::size_t buffer_size;

    /// @brief Creates a factory default configuration.
    /// @return A FeedConfig with sensible defaults for 3 symbols at 10k ticks/sec.
    static FeedConfig default_config() noexcept {
        return FeedConfig{
            {"BTCUSD", "ETHUSD", "SOLUSD"},
            {60000.0, 3000.0, 150.0},
            10000,
            0.001,
            1048576 // 2^20
        };
    }

    /// @brief Validates the configuration parameters.
    ///
    /// Checks that the symbols and initial_prices vectors are the same size,
    /// tick_rate_hz and volatility are strictly positive, and buffer_size is a
    /// power of two.
    /// @return true if the configuration is valid, false otherwise.
    [[nodiscard]] bool is_valid() const noexcept {
        if (symbols.size() != initial_prices.size()) {
            return false;
        }
        if (tick_rate_hz == 0) {
            return false;
        }
        if (volatility <= 0.0) {
            return false;
        }
        if (buffer_size == 0 || (buffer_size & (buffer_size - 1)) != 0) {
            return false;
        }
        return true;
    }
};

} // namespace mdp
