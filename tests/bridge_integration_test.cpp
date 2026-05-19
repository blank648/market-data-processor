// Copyright (c) 2026 Market Data Processor Project. All rights reserved.
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>
#include <pqxx/pqxx>

#include "infra/DbWriter.hpp"
#include "infra/Logger.hpp"
#include "core/RingBuffer.hpp"
#include "core/MarketSnapshot.hpp"

#include <chrono>
#include <cstring>
#include <thread>

using namespace mdp;
using namespace std::chrono_literals;

// Override via MDP_TEST_DB_CONN env var if the Docker port or credentials differ.
static const char* conn_str() {
    const char* env = std::getenv("MDP_TEST_DB_CONN");
    return env ? env
               : "host=localhost port=5433 dbname=marketdashboard "
                 "user=marketdashboard password=marketdashboard";
}

class BridgeIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        Logger::init("bridge-test", spdlog::level::warn);

        try {
            pqxx::connection probe(conn_str());
        } catch (const std::exception& ex) {
            GTEST_SKIP() << "PostgreSQL not reachable — skipping bridge test: " << ex.what();
        }

        test_start_s_ = std::chrono::duration<double>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    void TearDown() override {
        // Remove only the rows this test inserted (by time window).
        try {
            pqxx::connection conn(conn_str());
            pqxx::work w(conn);
            w.exec_params(
                R"(DELETE FROM "MarketPrices"
                   WHERE "Symbol" = $1 AND "RecordedAt" >= to_timestamp($2))",
                "AAPL", test_start_s_);
            w.commit();
        } catch (...) {}

        Logger::shutdown();
    }

    double test_start_s_{0.0};
};

// Verifies the full C++ → PostgreSQL write path:
// DbWriter must flush all 3 snapshots and the last-written AAPL price must be 151.0.
TEST_F(BridgeIntegrationTest, DbWriterFlushesSnapshotsToDB) {
    RingBuffer<MarketSnapshot, 4096> ring;

    // Push snapshots before starting the writer so the ring is ready immediately.
    const std::array<double, 3> prices = {150.0, 150.5, 151.0};
    for (double price : prices) {
        MarketSnapshot snap{};
        std::strncpy(snap.symbol.data(), "AAPL", snap.symbol.size());
        snap.price        = price;
        snap.volume       = 1000.0;
        snap.timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count();
        snap.sequence     = static_cast<int64_t>(price * 10);

        ASSERT_TRUE(ring.try_push(std::move(snap)));
        std::this_thread::sleep_for(2ms); // ensure distinct RecordedAt per row
    }

    DbWriter writer(ring, conn_str());
    writer.start();

    // DbWriter flushes every 10 ms; 500 ms gives substantial margin.
    std::this_thread::sleep_for(500ms);
    writer.stop(); // also triggers the final-batch flush added to DbWriter::run()

    // Query the most recently inserted AAPL row created during this test run.
    pqxx::connection conn(conn_str());
    pqxx::work w(conn);
    auto result = w.exec_params(
        R"(SELECT "Price"
           FROM "MarketPrices"
           WHERE "Symbol" = $1 AND "RecordedAt" >= to_timestamp($2)
           ORDER BY "Id" DESC
           LIMIT 1)",
        "AAPL", test_start_s_);
    w.commit();

    ASSERT_EQ(result.size(), 1u)
        << "DbWriter did not write any AAPL rows to MarketPrices";

    // numeric(18,6) is returned as a string by libpq; pqxx converts to double.
    EXPECT_DOUBLE_EQ(result[0][0].as<double>(), 151.0);
}
