// Copyright (c) 2026 Market Data Processor Project. All rights reserved.
// SPDX-License-Identifier: MIT

#include "feed/PostgresFeedReader.hpp"
#include "infra/Logger.hpp"

#include <pqxx/pqxx>
#include <algorithm>
#include <chrono>
#include <thread>

namespace mdp {

// ─── Construction ─────────────────────────────────────────────────────────────

PostgresFeedReader::PostgresFeedReader(FeedConfig config, std::string conn_string,
                                       TickRingBuffer16K& output)
    : ThreadBase<PostgresFeedReader>("PostgresFeedReader"),
      config_(std::move(config)),
      conn_string_(std::move(conn_string)),
      output_(output) {}

uint64_t PostgresFeedReader::ticks_published() const noexcept {
    return ticks_published_.load(std::memory_order_relaxed);
}

uint64_t PostgresFeedReader::ticks_dropped() const noexcept {
    return ticks_dropped_.load(std::memory_order_relaxed);
}

// ─── Internal helpers ─────────────────────────────────────────────────────────

namespace {

// Sleep for `total` in `slice`-sized chunks, honouring stop requests.
void interruptible_sleep(StopToken stop_token, std::chrono::milliseconds total,
                         std::chrono::milliseconds slice = std::chrono::milliseconds{100}) {
    auto remaining = total;
    while (remaining > std::chrono::milliseconds{0} && !stop_token.stop_requested()) {
        std::this_thread::sleep_for(std::min(slice, remaining));
        remaining -= slice;
    }
}

// Fetch one batch of AlphaVantage rows for `sym` newer than `last_seen_epoch`,
// push them into `output`, and advance `last_seen_epoch`.
void poll_symbol(pqxx::connection& conn, const std::string& sym, double& last_seen_epoch,
                 TickRingBuffer16K& output, std::atomic<uint64_t>& published,
                 std::atomic<uint64_t>& dropped, spdlog::logger& log) {
    pqxx::work txn(conn);

    auto rows = txn.exec_params(
        R"(SELECT "Price", "Volume",
                  EXTRACT(EPOCH FROM "RecordedAt") AS epoch_secs
           FROM "MarketPrices"
           WHERE "Source" = 1
             AND "Symbol" = $1
             AND EXTRACT(EPOCH FROM "RecordedAt") > $2
           ORDER BY "RecordedAt" ASC
           LIMIT 50)",
        sym, last_seen_epoch);

    txn.commit();

    for (const auto& row : rows) {
        auto price  = row[0].as<double>();
        auto volume = row[1].as<double>();
        auto epoch  = row[2].as<double>();

        last_seen_epoch = std::max(last_seen_epoch, epoch);

        // side=2 → trade; real price from Alpha Vantage, not simulated bid/ask
        auto tick = MarketTick::make(sym, price, volume, 2);
        if (output.try_push(tick)) {
            published.fetch_add(1, std::memory_order_relaxed);
            log.debug("Published: {} @ {:.4f}", sym, price);
        } else {
            dropped.fetch_add(1, std::memory_order_relaxed);
            log.warn("Ring buffer full — tick dropped for {}", sym);
        }
    }
}

} // namespace

// ─── Thread body ──────────────────────────────────────────────────────────────

void PostgresFeedReader::run(StopToken stop_token) {
    auto log = Logger::get("PostgresFeedReader");
    log->info("Starting. Watching {} symbols for Source=AlphaVantage (1) rows.",
              config_.symbols.size());

    // Epoch of the most-recently-published row; 0 picks up everything on first poll.
    double last_seen_epoch = 0.0;

    while (!stop_token.stop_requested()) {
        try {
            pqxx::connection conn(conn_string_);
            log->info("Connected to PostgreSQL.");

            while (!stop_token.stop_requested()) {
                try {
                    for (const auto& sym : config_.symbols) {
                        if (stop_token.stop_requested()) { break; }
                        poll_symbol(conn, sym, last_seen_epoch,
                                    output_, ticks_published_, ticks_dropped_, *log);
                    }
                } catch (const pqxx::broken_connection& e) {
                    log->warn("DB connection lost: {}. Reconnecting...", e.what());
                    break; // exit inner loop → reconnect
                } catch (const std::exception& e) {
                    log->error("Query error: {}", e.what());
                }

                // Alpha Vantage data arrives every ~60-130 s; polling every 2 s
                // keeps latency low without hammering the DB.
                interruptible_sleep(stop_token, std::chrono::milliseconds{2000});
            }
        } catch (const std::exception& e) {
            log->error("Failed to connect to DB: {}. Retrying in 5 s...", e.what());
            interruptible_sleep(stop_token, std::chrono::milliseconds{5000});
        }
    }

    log->info("Stopped. ticks_published={} ticks_dropped={}",
              ticks_published_.load(), ticks_dropped_.load());
}

}  // namespace mdp
