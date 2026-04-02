// Copyright (c) 2026 Market Data Processor Project. All rights reserved.
// SPDX-License-Identifier: MIT

#include "processing/Normalizer.hpp"

#include <thread>
#include <utility>

namespace mdp {

std::size_t Normalizer::SymbolHash::operator()(const std::array<char, 8>& s) const noexcept {
    // FNV-1a hash over the 8 bytes of symbol array
    uint64_t hash = 14695981039346656037ULL;
    for (char c : s) {
        hash ^= static_cast<uint8_t>(c);
        hash *= 1099511628211ULL;
    }
    return static_cast<std::size_t>(hash);
}

Normalizer::Normalizer(TickRingBuffer4K& input, TickRingBuffer4K& output)
    : ThreadBase("Normalizer"), input_(input), output_(output) {}

NormalizerStats::Snapshot Normalizer::stats() const noexcept {
    return stats_.snapshot();
}

void Normalizer::run(StopToken st) {
    MarketTick tick;
    while (!st.stop_requested()) {
        if (input_.try_pop(tick)) {
            if (is_duplicate(tick)) {
                stats_.ticks_deduplicated.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            if (is_reordered(tick)) {
                stats_.ticks_reordered.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            
            update_state(tick);
            
            // [COUNTER INTEGRITY] Only increment on confirmed push.
            // Tick lost during shutdown (stop requested mid-spin) is NOT counted.
            bool pushed = false;
            while (!st.stop_requested()) {
                if (output_.try_push(std::move(tick))) {
                    pushed = true;
                    break;
                }
                std::this_thread::yield();  // back-pressure: output full
            }
            
            if (pushed) {
                stats_.ticks_forwarded.fetch_add(1, std::memory_order_relaxed);
            }
        } else {
            std::this_thread::yield();  // nothing to read — yield to OS
        }
    }
}

bool Normalizer::is_duplicate(const MarketTick& tick) const noexcept {
    auto it = last_tick_.find(tick.symbol);
    if (it != last_tick_.end()) {
        if (it->second.price == tick.price && it->second.timestamp_ns == tick.timestamp_ns) {
            return true;
        }
    }
    return false;
}

bool Normalizer::is_reordered(const MarketTick& tick) const noexcept {
    auto it = last_timestamp_.find(tick.symbol);
    if (it != last_timestamp_.end()) {
        if (tick.timestamp_ns < it->second) {
            return true;
        }
    }
    return false;
}

void Normalizer::update_state(const MarketTick& tick) noexcept {
    last_timestamp_[tick.symbol] = tick.timestamp_ns;
    last_tick_[tick.symbol]      = tick;
}

}  // namespace mdp
