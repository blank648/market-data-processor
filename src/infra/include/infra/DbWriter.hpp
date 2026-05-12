// Copyright (c) 2026 Market Data Processor Project. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <chrono>
#include <libpq-fe.h>

#include "core/MarketTick.hpp"
#include "core/RingBuffer.hpp"
#include "core/ThreadBase.hpp"

namespace mdp {

/// @brief A pipeline stage that consumes MarketTick data and writes it to a PostgreSQL database.
class DbWriter final : public ThreadBase {
public:
    /// @brief Constructs the DbWriter.
    /// @param input The ring buffer to read ticks from (4K slots).
    /// @param connString The PostgreSQL connection string.
    DbWriter(TickRingBuffer4K& input, std::string_view connString);

    /// @brief Destructor. Ensures the thread is stopped and connection is closed.
    ~DbWriter() override;

protected:
    /// @brief The main worker loop for the thread.
    /// @param st A stop token for cooperative cancellation.
    void run(StopToken st) override;

private:
    /// @brief Attempts to connect to the PostgreSQL database.
    /// @return true if successful, false otherwise.
    bool connect();

    /// @brief Closes the connection to the database.
    void disconnect();

    /// @brief Flushes the current batch of ticks to the database.
    void flush_batch();

    TickRingBuffer4K& input_;
    std::string conn_string_;
    PGconn* conn_{nullptr};
    std::vector<MarketTick> batch_;

    static constexpr size_t BATCH_SIZE = 100;
    static constexpr std::chrono::seconds RETRY_INTERVAL{5};
    static constexpr std::chrono::seconds FLUSH_INTERVAL{1};
};

} // namespace mdp
