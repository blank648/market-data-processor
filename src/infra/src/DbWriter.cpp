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

void DbWriter::run(StopToken st) {
    auto log = Logger::get("DbWriter");
    log->info("Starting DbWriter thread...");

    while (!st.stop_requested()) {
        try {
            log->info("Connecting to PostgreSQL database...");
            pqxx::connection conn(conn_string_);
            log->info("Connected to database successfully!");

            // Prepare the upsert statement
            conn.prepare("upsert_price", 
                         "INSERT INTO MarketPrices (Symbol, Price, Timestamp) "
                         "VALUES ($1, $2, $3) "
                         "ON CONFLICT (Symbol) DO UPDATE SET Price = EXCLUDED.Price, Timestamp = EXCLUDED.Timestamp;");

            std::vector<MarketSnapshot> local_batch;
            local_batch.reserve(100);
            auto last_flush = std::chrono::steady_clock::now();

            // Keep connection open and check stop token
            while (!st.stop_requested()) {
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
        } catch (const std::exception& e) {
            log->error("Database connection failed: {}. Retrying in 5s...", e.what());
            // Retry with backoff sleep loop, checking stop_requested periodically
            for (int i = 0; i < 50; ++i) {
                if (st.stop_requested()) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    }

    log->info("DbWriter thread stopped.");
}

void DbWriter::flush_to_db(pqxx::connection& conn, const std::vector<MarketSnapshot>& batch) {
    pqxx::work w(conn);
    for (const auto& snap : batch) {
        std::string sym(snap.symbol.data(), ::strnlen(snap.symbol.data(), snap.symbol.size()));
        w.exec_prepared("upsert_price", sym, snap.price, snap.timestamp_ns);
    }
    w.commit();
}

} // namespace mdp
