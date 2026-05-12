// Copyright (c) 2026 Market Data Processor Project. All rights reserved.
// SPDX-License-Identifier: MIT

#include "infra/DbWriter.hpp"
#include "infra/Logger.hpp"
#include <thread>

namespace mdp {

DbWriter::DbWriter(TickRingBuffer4K& input, std::string_view connString)
    : ThreadBase("DbWriter"), input_(input), conn_string_(connString) {
    batch_.reserve(BATCH_SIZE);
}

DbWriter::~DbWriter() {
    ThreadBase::stop();
    disconnect();
}

void DbWriter::run(StopToken st) {
    auto log = Logger::get("DbWriter");
    log->info("Starting DbWriter loop...");

    auto last_flush = std::chrono::steady_clock::now();

    while (!st.stop_requested()) {
        if (!conn_) {
            if (!connect()) {
                // Wait 5 seconds before retrying, but check stop token
                for (int i = 0; i < 50; ++i) {
                    if (st.stop_requested()) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                continue;
            }
        }

        MarketTick tick;
        bool has_data = false;
        while (batch_.size() < BATCH_SIZE && input_.try_pop(tick)) {
            batch_.push_back(tick);
            has_data = true;
        }

        auto now = std::chrono::steady_clock::now();
        bool should_flush = (batch_.size() >= BATCH_SIZE) || 
                           (!batch_.empty() && (now - last_flush) >= FLUSH_INTERVAL);

        if (should_flush) {
            flush_batch();
            last_flush = now;
        }

        if (!has_data && batch_.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    // Final flush before exit
    if (!batch_.empty() && conn_) {
        flush_batch();
    }

    disconnect();
    log->info("DbWriter loop stopped.");
}

bool DbWriter::connect() {
    auto log = Logger::get("DbWriter");
    conn_ = PQconnectdb(conn_string_.c_str());

    if (PQstatus(conn_) != CONNECTION_OK) {
        log->error("Connection to database failed: {}", PQerrorMessage(conn_));
        disconnect();
        return false;
    }

    log->info("Successfully connected to PostgreSQL.");
    return true;
}

void DbWriter::disconnect() {
    if (conn_) {
        PQfinish(conn_);
        conn_ = nullptr;
    }
}

void DbWriter::flush_batch() {
    if (batch_.empty() || !conn_) return;

    auto log = Logger::get("DbWriter");
    
    // Use a transaction for the batch
    PGresult* res = PQexec(conn_, "BEGIN");
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        log->error("Failed to begin transaction: {}", PQerrorMessage(conn_));
        PQclear(res);
        return;
    }
    PQclear(res);

    const char* query =
        "INSERT INTO \"MarketPrices\" (\"Symbol\",\"Price\",\"Volume\",\"RecordedAt\",\"Source\",\"SymbolId\",\"CreatedAt\",\"UpdatedAt\") "
        "SELECT $1::varchar,$2::numeric,$3::numeric,NOW(),2,s.\"Id\",NOW(),NOW() "
        "FROM \"Symbols\" s WHERE s.\"Ticker\" = $1::varchar";

    bool success = true;
    for (const auto& tick : batch_) {
        // Prepare parameters
        std::string sym(tick.symbol.data(), tick.symbol.size());
        // Trim null bytes
        sym.erase(std::find(sym.begin(), sym.end(), '\0'), sym.end());
        
        std::string price_str = std::to_string(tick.price);
        std::string vol_str = std::to_string(tick.volume);

        const char* paramValues[3];
        paramValues[0] = sym.c_str();
        paramValues[1] = price_str.c_str();
        paramValues[2] = vol_str.c_str();

        res = PQexecParams(conn_, query, 3, nullptr, paramValues, nullptr, nullptr, 0);
        if (PQresultStatus(res) != PGRES_COMMAND_OK) {
            log->error("Insert failed: {}", PQerrorMessage(conn_));
            success = false;
            PQclear(res);
            break;
        }
        PQclear(res);
    }

    if (success) {
        res = PQexec(conn_, "COMMIT");
        if (PQresultStatus(res) != PGRES_COMMAND_OK) {
            log->error("Failed to commit transaction: {}", PQerrorMessage(conn_));
        }
        PQclear(res);
        batch_.clear();
    } else {
        res = PQexec(conn_, "ROLLBACK");
        PQclear(res);
        // We keep the batch to retry next time? 
        // Actually, if it's a persistent error (e.g. symbol not found), we might loop forever.
        // But the user didn't specify error handling for the insert itself, only connection.
        // For now, I'll clear it to avoid infinite loop on bad data, but in production we'd want more care.
        batch_.clear(); 
    }
}

} // namespace mdp
