// Copyright (c) 2026 Market Data Processor Project. All rights reserved.
// SPDX-License-Identifier: MIT

#include "processing/Normalizer.hpp"
#include "infra/Logger.hpp"
#include <thread>
#include <utility>

namespace mdp {

std::size_t Normalizer::SymbolHash::operator()(const std::array<char, 8>& symbol) const noexcept {
    // FNV-1a hash over the 8 bytes of symbol array
    uint64_t hash = 14695981039346656037ULL;
    for (char character : symbol) {
        hash ^= static_cast<uint8_t>(character);
        hash *= 1099511628211ULL;
    }
    return static_cast<std::size_t>(hash);
}

Normalizer::Normalizer(TickRingBuffer4K& input, TickRingBuffer4K& output)
    : ThreadBase("Normalizer"), input_(input), output_(output) {}

NormalizerStats::Snapshot Normalizer::stats() const noexcept {
    return stats_.snapshot();
}

void Normalizer::run(StopToken stop_token) {
    auto log_ = mdp::Logger::get("Normalizer");
    if (log_ != nullptr) {
        log_->info("Normalizer starting");
    }
    
    while (!stop_token.stop_requested()) {
        if (!process_tick(stop_token)) {
            std::this_thread::yield();  // nothing to read — yield to OS
        }
    }

    // Drain remaining ticks after stop is requested
    drain_input();
    
    if (log_ != nullptr) {
        log_->info("Normalizer stopped, stats: fwd={} dedup={} reorder={}", 
                   stats_.ticks_forwarded.load(std::memory_order_relaxed), 
                   stats_.ticks_deduplicated.load(std::memory_order_relaxed), 
                   stats_.ticks_reordered.load(std::memory_order_relaxed));
    }
}

bool Normalizer::process_tick(StopToken& stop_token) noexcept {
    MarketTick tick;
    if (!input_.try_pop(tick)) {
        return false;
    }
    
    if (is_duplicate(tick)) {
        stats_.ticks_deduplicated.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    if (is_reordered(tick)) {
        stats_.ticks_reordered.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    
    update_state(tick);
    
    auto log_ = mdp::Logger::get("Normalizer");
    if (log_ != nullptr) {
        log_->trace("Forwarding tick: {}", tick.to_string());
    }
    
    // [COUNTER INTEGRITY] Only increment on confirmed push.
    // Tick lost during shutdown (stop requested mid-spin) is NOT counted.
    bool pushed = false;
    while (!stop_token.stop_requested()) {
        if (output_.try_push(tick)) {
            pushed = true;
            break;
        }
        std::this_thread::yield();  // back-pressure: output full
    }
    
    if (pushed) {
        stats_.ticks_forwarded.fetch_add(1, std::memory_order_relaxed);
    }
    return true;
}

void Normalizer::drain_input() noexcept {
    MarketTick tick;
    auto log_ = mdp::Logger::get("Normalizer");
    
    while (input_.try_pop(tick)) {
        if (is_duplicate(tick)) {
            stats_.ticks_deduplicated.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        if (is_reordered(tick)) {
            stats_.ticks_reordered.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        
        update_state(tick);
        if (log_ != nullptr) {
            log_->trace("Forwarding tick: {}", tick.to_string());
        }
        
        // Ignore back-pressure and just try to push; drop if full
        if (output_.try_push(tick)) {
            stats_.ticks_forwarded.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

bool Normalizer::is_duplicate(const MarketTick& tick) const noexcept {
    auto iter = last_tick_.find(tick.symbol);
    if (iter != last_tick_.end()) {
        if (iter->second.price == tick.price && iter->second.timestamp_ns == tick.timestamp_ns) {
            return true;
        }
    }
    return false;
}

bool Normalizer::is_reordered(const MarketTick& tick) const noexcept {
    auto iter = last_timestamp_.find(tick.symbol);
    if (iter != last_timestamp_.end()) {
        if (tick.timestamp_ns < iter->second) {
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
