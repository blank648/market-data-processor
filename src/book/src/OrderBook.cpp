// Copyright (c) 2026 Market Data Processor Project. All rights reserved.
// SPDX-License-Identifier: MIT

#include "book/OrderBook.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <iterator>
#include <string>

namespace mdp {

OrderBook::OrderBook(std::string_view symbol) {
    symbol_.fill('\0');
    const std::string safe_symbol(symbol);
    std::strncpy(symbol_.data(), safe_symbol.c_str(), symbol_.size() - 1);
    symbol_.back() = '\0';
}

void OrderBook::apply(const BookDelta& delta) noexcept {
    // Hot path: called at high frequency; keep logic branch-simple and allocation-free.
    // noexcept is critical here: throwing in the hot path would terminate the process.
    ++sequence_;
    ++updates_applied_;

    // [CROSSED BOOK GUARD] Reject genuinely crossed books (bid > best ask, or
    // ask < best bid) which indicate a feed error. Locked books (bid == ask)
    // are accepted: Alpha Vantage supplies a single last-trade price, so bid
    // and ask collapse to the same level — that is valid "last price" data,
    // not a feed error.
    if (delta.volume > 0) {
        if (delta.side == OrderSide::BID && !asks_.empty() && delta.price > asks_.begin()->first) {
            const double best_ask = asks_.begin()->first;
            std::cerr << "[BOOK WARN] Crossed book rejected for " << symbol()
                      << ": bid " << delta.price << " > best ask " << best_ask << "\n";
            return;
        }

        if (delta.side == OrderSide::ASK && !bids_.empty() && delta.price < bids_.rbegin()->first) {
            const double best_bid = bids_.rbegin()->first;
            std::cerr << "[BOOK WARN] Crossed book rejected for " << symbol()
                      << ": ask " << delta.price << " < best bid " << best_bid << "\n";
            return;
        }
    }

    if (delta.side == OrderSide::BID) {
        if (delta.volume == 0) {
            levels_removed_ += bids_.erase(delta.price);
            return;
        }
        bids_[delta.price] = delta.volume;
        return;
    }

    if (delta.volume == 0) {
        levels_removed_ += asks_.erase(delta.price);
        return;
    }

    asks_[delta.price] = delta.volume;
}

TopOfBook OrderBook::top_of_book() const noexcept {
    TopOfBook tob{};
    tob.symbol = symbol();

    if (!bids_.empty()) {
        const auto iter = bids_.rbegin();
        tob.best_bid = iter->first;
        tob.bid_volume = iter->second;
    }

    if (!asks_.empty()) {
        const auto iter = asks_.begin();
        tob.best_ask = iter->first;
        tob.ask_volume = iter->second;
    }

    if (tob.best_bid > 0.0 && tob.best_ask > 0.0) {
        tob.spread = tob.best_ask - tob.best_bid;
    }

    tob.timestamp_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    return tob;
}

BookSnapshot OrderBook::snapshot() const {
    // Snapshot path is diagnostic/test oriented and may allocate vector storage.
    BookSnapshot snap{};
    std::strncpy(snap.symbol.data(), symbol_.data(), snap.symbol.size() - 1);
    snap.symbol.back() = '\0';
    snap.timestamp_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    snap.sequence = sequence_;

    snap.bids.reserve(bids_.size());
    for (auto iter = bids_.rbegin(); iter != bids_.rend(); ++iter) {
        snap.bids.push_back(PriceLevel{iter->first, iter->second});
    }

    snap.asks.reserve(asks_.size());
    std::ranges::transform(asks_, std::back_inserter(snap.asks),
                           [](const auto& level) { return PriceLevel{level.first, level.second}; });

    return snap;
}

std::size_t OrderBook::bid_levels() const noexcept {
    return bids_.size();
}

std::size_t OrderBook::ask_levels() const noexcept {
    return asks_.size();
}

void OrderBook::clear() noexcept {
    bids_.clear();
    asks_.clear();
    sequence_ = 0;
    updates_applied_ = 0;
    levels_removed_ = 0;
}

uint64_t OrderBook::updates_applied() const noexcept {
    return updates_applied_;
}

uint64_t OrderBook::levels_removed() const noexcept {
    return levels_removed_;
}

std::string_view OrderBook::symbol() const noexcept {
    return std::string_view(symbol_.data());
}

}  // namespace mdp


