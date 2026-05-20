// Copyright (c) 2026 Market Data Processor Project. All rights reserved.
// SPDX-License-Identifier: MIT

#include "infra/DbWriter.hpp"
#include "infra/Logger.hpp"
#include <thread>
#include <chrono>
#include <cstring>

namespace mdp {

DbWriter::DbWriter(RingBuffer<MarketSnapshot, 4096>& input, const std::string& conn_string)
    : ThreadBase<DbWriter>("DbWriter"), input_(input), conn_string_(conn_string) {}

DbWriter::~DbWriter() {
    stop();
}

void DbWriter::run(StopToken stop_token) {
    auto log = Logger::get("DbWriter");
    log->info("Starting DbWriter thread...");

    while (!stop_token.stop_requested()) {
        try {
            log->info("Connecting to PostgreSQL database...");
            pqxx::connection conn(conn_string_);
            log->info("Connected to database successfully!");

            // INSERT with full schema: joins Symbols to resolve SymbolId, Source=2 (C++ processor).
            // Uses snap.timestamp_ns (converted to seconds) for RecordedAt.
            conn.prepare("insert_price",
                         R"(INSERT INTO "MarketPrices"
                              ("Symbol","Price","Volume","RecordedAt","Source","SymbolId","CreatedAt","UpdatedAt")
                            SELECT $1::varchar, $2::numeric, $3::bigint, to_timestamp($4), 2, s."Id", NOW(), NOW()
                            FROM "Symbols" s WHERE s."Ticker" = $1::varchar)");

            process_queue(conn, stop_token);
        } catch (const std::exception& e) {
            log->error("Database connection failed: {}. Retrying in 5s...", e.what());
            // Retry with backoff sleep loop, checking stop_requested periodically
            for (int i = 0; i < 50; ++i) {
                if (stop_token.stop_requested()) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    }

    log->info("DbWriter thread stopped.");
}

void DbWriter::process_queue(pqxx::connection& conn, StopToken stop_token) {
    std::vector<MarketSnapshot> local_batch;
    local_batch.reserve(100);
    auto last_flush = std::chrono::steady_clock::now();

    // Keep connection open and check stop token
    while (!stop_token.stop_requested()) {
        MarketSnapshot snap;
        if (input_.try_pop(snap)) {
            local_batch.push_back(snap);
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        auto now = std::chrono::steady_clock::now();
        if (local_batch.size() >= 100 || std::chrono::duration_cast<std::chrono::milliseconds>(now - last_flush).count() >= 10) {
            if (!local_batch.empty()) {
                flush_to_db(conn, local_batch);
                local_batch.clear();
            }
            last_flush = now;
        }
    }

    // Flush any items accumulated since the last periodic window before exiting.
    if (!local_batch.empty()) {
        flush_to_db(conn, local_batch);
        local_batch.clear();
    }
}

void DbWriter::flush_to_db(pqxx::connection& conn, const std::vector<MarketSnapshot>& batch) {
    pqxx::work txn(conn);
    for (const auto& snap : batch) {
        std::string sym(snap.symbol.data(), ::strnlen(snap.symbol.data(), snap.symbol.size()));
        txn.exec_prepared("insert_price",
            sym,
            snap.price,
            static_cast<int64_t>(snap.volume),
            static_cast<double>(snap.timestamp_ns) / 1'000'000'000.0);
    }
    txn.commit();
}

} // namespace mdp
