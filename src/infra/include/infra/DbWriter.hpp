// Copyright (c) 2026 Market Data Processor Project. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <string>
#include <pqxx/pqxx>

#include "core/MarketSnapshot.hpp"
#include "core/RingBuffer.hpp"
#include "core/ThreadBase.hpp"

#include <vector>

namespace mdp {

/// @brief A pipeline stage that consumes MarketSnapshot data and writes it to a PostgreSQL database.
class DbWriter final : public ThreadBase<DbWriter> {
public:
    /// @brief Constructs the DbWriter.
    /// @param input The ring buffer to read snapshots from (4K slots).
    /// @param conn_string The PostgreSQL connection string.
    DbWriter(RingBuffer<MarketSnapshot, 4096>& input, const std::string& conn_string);

    /// @brief Destructor. Ensures the thread is stopped.
    ~DbWriter() override;

    /// @brief The main worker loop for the thread.
    /// @param st A stop token for cooperative cancellation.
    void run(StopToken st);

private:
    void flush_to_db(pqxx::connection& conn, const std::vector<MarketSnapshot>& batch);

    RingBuffer<MarketSnapshot, 4096>& input_;
    std::string conn_string_;
};

} // namespace mdp
