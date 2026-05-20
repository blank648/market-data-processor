// Copyright (c) 2026 Market Data Processor Project. All rights reserved.
// SPDX-License-Identifier: MIT

#include "infra/SignalDbWriter.hpp"
#include "infra/Logger.hpp"
#include <thread>
#include <chrono>
#include <cstring>

namespace mdp {

SignalDbWriter::SignalDbWriter(RingBuffer<Signal, 4096>& input, const std::string& conn_string)
    : ThreadBase<SignalDbWriter>("SignalDbWriter"), input_(input), conn_string_(conn_string) {}

SignalDbWriter::~SignalDbWriter() {
    stop();
}

void SignalDbWriter::run(StopToken stop_token) {
    auto log = Logger::get("SignalDbWriter");
    log->info("Starting SignalDbWriter thread...");

    while (!stop_token.stop_requested()) {
        try {
            log->info("Connecting to PostgreSQL database...");
            pqxx::connection conn(conn_string_);
            log->info("Connected to database successfully!");

            // Prepared statement for StrategySignals: joins Symbols to resolve SymbolId.
            // Converting type to string, using NOW() for CreatedAt.
            conn.prepare("insert_signal",
                         R"(INSERT INTO "StrategySignals"
                               ("SymbolId","SignalType","Price","Timestamp","CreatedAt")
                             SELECT s."Id", $2::varchar, $3::numeric, to_timestamp($4), NOW()
                             FROM "Symbols" s WHERE s."Ticker" = $1::varchar)");

            process_queue(conn, stop_token);
        } catch (const std::exception& e) {
            log->error("Database connection failed: {}. Retrying in 5s...", e.what());
            for (int i = 0; i < 50; ++i) {
                if (stop_token.stop_requested()) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    }

    log->info("SignalDbWriter thread stopped.");
}

void SignalDbWriter::process_queue(pqxx::connection& conn, StopToken stop_token) {
    auto log = Logger::get("SignalDbWriter");
    std::vector<Signal> local_batch;
    local_batch.reserve(100);
    auto last_flush = std::chrono::steady_clock::now();

    // Keep connection open and check stop token
    while (!stop_token.stop_requested()) {
        Signal sig;
        if (input_.try_pop(sig)) {
            if (sig.type == SignalType::BUY || sig.type == SignalType::SELL) {
                std::string type_str = (sig.type == SignalType::BUY) ? "BUY" : "SELL";
                log->info("[SIGNAL] {} -> {} @ {:.2f}", sig.symbol_view(), type_str, sig.price);
            }
            local_batch.push_back(sig);
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

    // Flush any remaining items before exiting
    if (!local_batch.empty()) {
        flush_to_db(conn, local_batch);
        local_batch.clear();
    }
}

void SignalDbWriter::flush_to_db(pqxx::connection& conn, const std::vector<Signal>& batch) {
    pqxx::work txn(conn);
    for (const auto& sig : batch) {
        std::string sym(sig.symbol.data(), ::strnlen(sig.symbol.data(), sig.symbol.size()));
        std::string type_str = "HOLD";
        if (sig.type == SignalType::BUY) {
            type_str = "BUY";
        } else if (sig.type == SignalType::SELL) {
            type_str = "SELL";
        }

        txn.exec_prepared("insert_signal",
            sym,
            type_str,
            sig.price,
            static_cast<double>(sig.timestamp_ns) / 1'000'000'000.0);
    }
    txn.commit();
}

} // namespace mdp
