// Copyright (c) 2026 Market Data Processor Project. All rights reserved.
// SPDX-License-Identifier: MIT

#include "processing/TickParser.hpp"
#include "infra/Logger.hpp"
#include <algorithm>
#include <cmath>
#include <thread>

namespace mdp {

TickParser::TickParser(TickRingBuffer16K& input, TickRingBuffer4K& output)
    : ThreadBase("TickParser"), input_(input), output_(output) {}

uint64_t TickParser::ticks_processed() const noexcept {
    return ticks_processed_.load(std::memory_order_relaxed);
}

uint64_t TickParser::ticks_rejected() const noexcept {
    return ticks_rejected_.load(std::memory_order_relaxed);
}

void TickParser::run(StopToken stop_token) {
    auto log_ = mdp::Logger::get("TickParser");
    if (log_ != nullptr) {
        log_->info("TickParser starting");
    }
    
    while (!stop_token.stop_requested()) {
        if (!process_tick(stop_token)) {
            std::this_thread::yield();  // nothing to read — yield to OS
        }
    }

    // Drain remaining ticks after stop is requested
    drain_input();
    
    if (log_ != nullptr) {
        log_->info("TickParser stopped, ticks_parsed={}", ticks_processed_.load(std::memory_order_relaxed));
    }
}

bool TickParser::process_tick(StopToken& stop_token) noexcept {
    MarketTick tick;
    if (!input_.try_pop(tick)) {
        return false;
    }
    
    enrich(tick);  // normalize before validation so side-clamping is reachable
    if (validate(tick)) {
        auto log_ = mdp::Logger::get("TickParser");
        if (log_ != nullptr) {
            log_->trace("Parsed tick: {}", tick.to_string());
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
            ticks_processed_.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        ticks_rejected_.fetch_add(1, std::memory_order_relaxed);
    }
    return true;
}

void TickParser::drain_input() noexcept {
    MarketTick tick;
    auto log_ = mdp::Logger::get("TickParser");
    
    while (input_.try_pop(tick)) {
        enrich(tick);  // normalize before validation
        if (validate(tick)) {
            if (log_ != nullptr) {
                log_->trace("Parsed tick: {}", tick.to_string());
            }
            // Ignore back-pressure and just try to push; drop if full
            if (output_.try_push(tick)) {
                ticks_processed_.fetch_add(1, std::memory_order_relaxed);
            }
        } else {
            ticks_rejected_.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

bool TickParser::validate(const MarketTick& tick) noexcept {
    if (tick.price <= 0.0 || std::isnan(tick.price) || std::isinf(tick.price)) {
        return false;
    }
    if (tick.volume <= 0.0 || std::isnan(tick.volume)) {
        return false;
    }
    if (tick.timestamp_ns <= 0) {
        return false;
    }
    if (tick.side > 2) {
        return false;
    }
    if (tick.symbol[0] == '\0') {
        return false;
    }
    return true;
}

void TickParser::enrich(MarketTick& tick) noexcept {
    tick.symbol[7] = '\0';
    tick.side = std::min<std::uint8_t>(tick.side, 2);
}

}  // namespace mdp
