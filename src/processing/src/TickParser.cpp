// Copyright (c) 2026 Market Data Processor Project. All rights reserved.
// SPDX-License-Identifier: MIT

#include "processing/TickParser.hpp"

#include <cmath>
#include <thread>
#include <utility>

namespace mdp {

TickParser::TickParser(TickRingBuffer16K& input, TickRingBuffer4K& output)
    : ThreadBase("TickParser"), input_(input), output_(output) {}

uint64_t TickParser::ticks_processed() const noexcept {
    return ticks_processed_.load(std::memory_order_relaxed);
}

uint64_t TickParser::ticks_rejected() const noexcept {
    return ticks_rejected_.load(std::memory_order_relaxed);
}

void TickParser::run(StopToken st) {
    MarketTick tick;
    while (!st.stop_requested()) {
        if (input_.try_pop(tick)) {
            if (validate(tick)) {
                enrich(tick);
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
                    ticks_processed_.fetch_add(1, std::memory_order_relaxed);
                }
            } else {
                ticks_rejected_.fetch_add(1, std::memory_order_relaxed);
            }
        } else {
            std::this_thread::yield();  // nothing to read — yield to OS
        }
    }

    // Drain remaining ticks after stop is requested
    while (input_.try_pop(tick)) {
        if (validate(tick)) {
            enrich(tick);
            // Ignore back-pressure and just try to push; drop if full
            if (output_.try_push(std::move(tick))) {
                ticks_processed_.fetch_add(1, std::memory_order_relaxed);
            }
        } else {
            ticks_rejected_.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

bool TickParser::validate(const MarketTick& tick) const noexcept {
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

void TickParser::enrich(MarketTick& tick) const noexcept {
    tick.symbol[7] = '\0';
    if (tick.side > 2) {
        tick.side = 2;
    }
}

}  // namespace mdp
