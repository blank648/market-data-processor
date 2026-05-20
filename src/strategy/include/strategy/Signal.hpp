#pragma once

#include <algorithm>
#include <cstdint>
#include <array>
#include <string_view>

namespace mdp {

enum class SignalType : uint8_t {
    HOLD = 0,
    BUY = 1,
    SELL = 2
};

struct alignas(64) Signal {
    std::array<char, 8> symbol{};
    double price{0.0};
    double ema_value{0.0};
    int64_t timestamp_ns{0};
    SignalType type{SignalType::HOLD};

    // Helper for fast symbol matching/printing
    std::string_view symbol_view() const noexcept {
        const auto* const terminator = std::find(symbol.begin(), symbol.end(), '\0');
        return std::string_view(symbol.data(), static_cast<size_t>(std::distance(symbol.begin(), terminator)));
    }
};

static_assert(sizeof(Signal) == 64, "Signal must be exactly 64 bytes (cache line size) for optimal queue performance");

} // namespace mdp
