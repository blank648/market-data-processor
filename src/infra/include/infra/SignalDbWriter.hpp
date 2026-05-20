// Copyright (c) 2026 Market Data Processor Project. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <string>
#include <pqxx/pqxx>
#include <vector>

#include "strategy/Signal.hpp"
#include "core/RingBuffer.hpp"
#include "core/ThreadBase.hpp"

namespace mdp {

/// @brief A pipeline stage that consumes strategy Signal data and writes it to a PostgreSQL database.
class SignalDbWriter final : public ThreadBase<SignalDbWriter> {
public:
    SignalDbWriter(RingBuffer<Signal, 4096>& input, const std::string& conn_string);

    // Non-copyable and non-movable (Rule of Five compliance)
    SignalDbWriter(const SignalDbWriter&) = delete;
    SignalDbWriter& operator=(const SignalDbWriter&) = delete;
    SignalDbWriter(SignalDbWriter&&) = delete;
    SignalDbWriter& operator=(SignalDbWriter&&) = delete;

    /// @brief Destructor. Ensures the thread is stopped.
    ~SignalDbWriter() override;

    /// @brief The main worker loop for the thread.
    /// @param stop_token A stop token for cooperative cancellation.
    void run(StopToken stop_token);

private:
    void process_queue(pqxx::connection& conn, StopToken stop_token);
    static void flush_to_db(pqxx::connection& conn, const std::vector<Signal>& batch);

    RingBuffer<Signal, 4096>& input_;
    std::string conn_string_;
};

} // namespace mdp
