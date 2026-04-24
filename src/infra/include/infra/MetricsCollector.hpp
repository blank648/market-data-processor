// Copyright (c) 2026 Market Data Processor Project. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include "feed/FeedSimulator.hpp"
#include "processing/TickParser.hpp"
#include "processing/Normalizer.hpp"
#include "book/BookProcessor.hpp"
#include "infra/Logger.hpp"
#include <nlohmann/json.hpp>
#include <chrono>

namespace mdp {

struct PipelineSnapshot {
    // Feed
    uint64_t feed_ticks_published;
    uint64_t feed_ticks_dropped;

    // Parser
    uint64_t parser_ticks_processed;
    uint64_t parser_ticks_rejected;

    // Normalizer
    uint64_t norm_ticks_forwarded;
    uint64_t norm_ticks_deduplicated;
    uint64_t norm_ticks_reordered;

    // Book
    uint64_t book_ticks_processed;
    uint64_t book_books_active;

    // Timestamp
    int64_t snapshot_time_ns;

    [[nodiscard]] double feed_drop_rate() const noexcept {
        return feed_ticks_published > 0 ? static_cast<double>(feed_ticks_dropped) / static_cast<double>(feed_ticks_published) : 0.0;
    }

    [[nodiscard]] double parser_reject_rate() const noexcept {
        return parser_ticks_processed > 0 ? static_cast<double>(parser_ticks_rejected) / static_cast<double>(parser_ticks_processed) : 0.0;
    }

    [[nodiscard]] double norm_dedup_rate() const noexcept {
        uint64_t total = norm_ticks_forwarded + norm_ticks_deduplicated + norm_ticks_reordered;
        return total > 0 ? static_cast<double>(norm_ticks_deduplicated) / static_cast<double>(total) : 0.0;
    }
};

class MetricsCollector {
public:
    MetricsCollector(
        const FeedSimulator&  feed,
        const TickParser&     parser,
        const Normalizer&     normalizer,
        const BookProcessor&  book
    ) : feed_(feed), parser_(parser), normalizer_(normalizer), book_(book) {}

    [[nodiscard]] PipelineSnapshot snapshot() const noexcept {
        PipelineSnapshot snap{};
        snap.feed_ticks_published = feed_.ticks_published();
        snap.feed_ticks_dropped   = feed_.ticks_dropped();
        snap.parser_ticks_processed = parser_.ticks_processed();
        snap.parser_ticks_rejected  = parser_.ticks_rejected();

        auto norm_stats = normalizer_.stats();
        snap.norm_ticks_forwarded = norm_stats.ticks_forwarded;
        snap.norm_ticks_deduplicated = norm_stats.ticks_deduplicated;
        snap.norm_ticks_reordered = norm_stats.ticks_reordered;

        snap.book_ticks_processed = book_.ticks_processed();
        snap.book_books_active = book_.books_active();

        snap.snapshot_time_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch()
                                ).count();

        return snap;
    }

    [[nodiscard]] nlohmann::json to_json() const {
        auto snap = snapshot();
        return nlohmann::json{
            {"snapshot_time_ns", snap.snapshot_time_ns},
            {"feed", {
                {"ticks_published", snap.feed_ticks_published},
                {"ticks_dropped", snap.feed_ticks_dropped}
            }},
            {"parser", {
                {"ticks_processed", snap.parser_ticks_processed},
                {"ticks_rejected", snap.parser_ticks_rejected}
            }},
            {"normalizer", {
                {"ticks_forwarded", snap.norm_ticks_forwarded},
                {"ticks_deduplicated", snap.norm_ticks_deduplicated},
                {"ticks_reordered", snap.norm_ticks_reordered}
            }},
            {"book", {
                {"ticks_processed", snap.book_ticks_processed},
                {"books_active", snap.book_books_active}
            }},
            {"derived", {
                {"feed_drop_rate", snap.feed_drop_rate()},
                {"parser_reject_rate", snap.parser_reject_rate()},
                {"norm_dedup_rate", snap.norm_dedup_rate()}
            }}
        };
    }

    void to_log() const {
        auto snap = snapshot();
        auto log_ = mdp::Logger::get("MetricsCollector");
        if (log_) {
            log_->info("feed={}/{} parser={}/{} norm={}/{}/{} book={}/{}",
                snap.feed_ticks_published, snap.feed_ticks_dropped,
                snap.parser_ticks_processed, snap.parser_ticks_rejected,
                snap.norm_ticks_forwarded, snap.norm_ticks_deduplicated, snap.norm_ticks_reordered,
                snap.book_ticks_processed, snap.book_books_active
            );
        }
    }

private:
    const FeedSimulator&  feed_;
    const TickParser&     parser_;
    const Normalizer&     normalizer_;
    const BookProcessor&  book_;
};

} // namespace mdp
