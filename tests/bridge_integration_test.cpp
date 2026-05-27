// Copyright (c) 2026 Market Data Processor Project. All rights reserved.
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>
#include <pqxx/pqxx>

#include "infra/DbWriter.hpp"
#include "infra/SignalDbWriter.hpp"
#include "infra/Logger.hpp"
#include "core/RingBuffer.hpp"
#include "core/MarketSnapshot.hpp"
#include "strategy/Signal.hpp"
#include "feed/FeedConfig.hpp"
#include "feed/PostgresFeedReader.hpp"
#include "core/MarketTick.hpp"

#include <chrono>
#include <cstring>
#include <thread>

using namespace mdp;
using namespace std::chrono_literals;

// Override via MDP_TEST_DB_CONN env var if the Docker port or credentials differ.
static const char* conn_str() {
    const char* env = std::getenv("MDP_TEST_DB_CONN");
    return env != nullptr ? env
                          : "host=localhost port=5433 dbname=marketdashboard "
                            "user=marketdashboard password=marketdashboard";
}

class BridgeIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        Logger::init("bridge-test", spdlog::level::warn);

        try {
            pqxx::connection probe(conn_str());
            
            // Create StrategySignals table if it doesn't exist to enable testing Option A
            pqxx::work txn(probe);
            txn.exec(R"(
                CREATE TABLE IF NOT EXISTS "StrategySignals" (
                    "Id" SERIAL PRIMARY KEY,
                    "SymbolId" INT,
                    "SignalType" VARCHAR(10) NOT NULL,
                    "Price" NUMERIC NOT NULL,
                    "Timestamp" TIMESTAMP NOT NULL,
                    "CreatedAt" TIMESTAMP NOT NULL
                );
            )");
            txn.commit();
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
            pqxx::work txn(conn);
            txn.exec_params(
                R"(DELETE FROM "MarketPrices"
                   WHERE "Symbol" = $1 AND "RecordedAt" >= to_timestamp($2))",
                "AAPL", test_start_s_);
            txn.exec_params(
                R"(DELETE FROM "StrategySignals"
                   WHERE "Timestamp" >= to_timestamp($1))",
                test_start_s_);
            txn.commit();
        } catch (...) {}

        Logger::shutdown();
    }

    double get_test_start_s() const { return test_start_s_; }

private:
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
        ASSERT_TRUE(ring.try_push(snap));
        std::this_thread::sleep_for(2ms); // ensure distinct RecordedAt per row
    }

    DbWriter writer(ring, conn_str());
    writer.start();

    // DbWriter flushes every 10 ms; 500 ms gives substantial margin.
    std::this_thread::sleep_for(500ms);
    writer.stop(); // also triggers the final-batch flush added to DbWriter::run()

    // Query the most recently inserted AAPL row created during this test run.
    pqxx::connection conn(conn_str());
    pqxx::work txn(conn);
    auto result = txn.exec_params(
        R"(SELECT "Price"
           FROM "MarketPrices"
           WHERE "Symbol" = $1 AND "RecordedAt" >= to_timestamp($2)
           ORDER BY "Id" DESC
           LIMIT 1)",
        "AAPL", get_test_start_s());
    txn.commit();

    ASSERT_EQ(result.size(), 1U)
        << "DbWriter did not write any AAPL rows to MarketPrices";

    // numeric(18,6) is returned as a string by libpq; pqxx converts to double.
    EXPECT_DOUBLE_EQ(result[0][0].as<double>(), 151.0);
}

TEST_F(BridgeIntegrationTest, SignalDbWriterFlushesSignalsToDB) {
    RingBuffer<Signal, 4096> ring;

    // Push signals
    const std::array<double, 3> prices = {150.0, 150.5, 151.0};
    const std::array<SignalType, 3> types = {SignalType::HOLD, SignalType::BUY, SignalType::SELL};
    for (size_t i = 0; i < 3; ++i) {
        Signal sig{};
        std::strncpy(sig.symbol.data(), "AAPL", sig.symbol.size());
        sig.price = prices.at(i);
        sig.ema_value = prices.at(i) - 0.2;
        sig.timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
        sig.type = types.at(i);
        ASSERT_TRUE(ring.try_push(sig));
        std::this_thread::sleep_for(2ms);
    }

    SignalDbWriter writer(ring, conn_str());
    writer.start();

    std::this_thread::sleep_for(500ms);
    writer.stop();

    pqxx::connection conn(conn_str());
    pqxx::work txn(conn);
    auto result = txn.exec_params(
        R"(SELECT "Price", "SignalType"
           FROM "StrategySignals"
           WHERE "Timestamp" >= to_timestamp($1)
           ORDER BY "Id" ASC)",
        get_test_start_s());
    txn.commit();

    ASSERT_EQ(result.size(), 3U)
        << "SignalDbWriter did not write 3 signals to StrategySignals";

    EXPECT_DOUBLE_EQ(result[0][0].as<double>(), 150.0);
    EXPECT_EQ(result[0][1].as<std::string>(), "HOLD");

    EXPECT_DOUBLE_EQ(result[1][0].as<double>(), 150.5);
    EXPECT_EQ(result[1][1].as<std::string>(), "BUY");

    EXPECT_DOUBLE_EQ(result[2][0].as<double>(), 151.0);
    EXPECT_EQ(result[2][1].as<std::string>(), "SELL");
}

// ─────────────────────────────────────────────────────────────────────────────
// PostgresFeedReaderBridgeTest
// Requires a live PostgreSQL instance. Inserts test rows with a synthetic ticker
// "PRFT" (unique, non-colliding) and verifies that PostgresFeedReader picks them
// up correctly. TearDown removes all inserted rows and the test symbol.
// ─────────────────────────────────────────────────────────────────────────────

class PostgresFeedReaderBridgeTest : public ::testing::Test {
protected:
    static constexpr const char* kTestTicker = "PRFT";

    void SetUp() override {
        Logger::init("pfr-bridge-test", spdlog::level::warn);

        try {
            pqxx::connection probe(conn_str());

            pqxx::work txn(probe);
            // Insert a synthetic Symbol so MarketPrices FK is satisfied.
            txn.exec_params(
                R"(INSERT INTO "Symbols" ("Ticker","CompanyName","IsActive","CreatedAt","UpdatedAt")
                   VALUES ($1,'PostgresFeedReader Test Symbol',false,NOW(),NOW())
                   ON CONFLICT ("Ticker") DO NOTHING)",
                kTestTicker);
            txn.commit();
        } catch (const std::exception& ex) {
            GTEST_SKIP() << "PostgreSQL not reachable — skipping PostgresFeedReader bridge tests: "
                         << ex.what();
        }
    }

    void TearDown() override {
        try {
            pqxx::connection conn(conn_str());
            pqxx::work txn(conn);
            txn.exec_params(
                R"(DELETE FROM "MarketPrices" WHERE "Symbol" = $1)", kTestTicker);
            txn.exec_params(
                R"(DELETE FROM "Symbols" WHERE "Ticker" = $1)", kTestTicker);
            txn.commit();
        } catch (...) {}
        Logger::shutdown();
    }

    // Helper: insert a MarketPrices row for kTestTicker with Source=1 (AlphaVantage).
    void insert_price_row(pqxx::connection& conn, double price, double volume,
                          const std::string& recorded_at_expr = "NOW()") {
        pqxx::work txn(conn);
        // Use a subquery so SymbolId is resolved correctly even when the caller
        // doesn't know the numeric Id.
        txn.exec_params(
            R"(INSERT INTO "MarketPrices"
                   ("Symbol","Price","Volume","RecordedAt","Source","SymbolId","CreatedAt","UpdatedAt")
               SELECT $1::varchar, $2::numeric, $3::bigint, )" + recorded_at_expr + R"(, 1, s."Id", NOW(), NOW()
               FROM "Symbols" s WHERE s."Ticker" = $1::varchar)",
            kTestTicker, price, static_cast<long long>(volume));
        txn.commit();
    }
};

// Verifies basic read path: rows inserted before start are published on first poll.
TEST_F(PostgresFeedReaderBridgeTest, PublishesAlphaVantageRowsOnFirstPoll) {
    {
        pqxx::connection conn(conn_str());
        insert_price_row(conn, 123.45, 5000.0, "NOW() - INTERVAL '10 seconds'");
        insert_price_row(conn, 234.56, 6000.0, "NOW() - INTERVAL '5 seconds'");
    }

    TickRingBuffer16K output;
    FeedConfig cfg;
    cfg.symbols = {kTestTicker};

    PostgresFeedReader reader(cfg, conn_str(), output);
    reader.start();

    // First poll fires immediately; wait up to 3s.
    MarketTick tick{};
    std::vector<MarketTick> received;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        if (output.try_pop(tick)) received.push_back(tick);
        if (received.size() >= 2) break;
        std::this_thread::yield();
    }
    reader.stop();
    while (output.try_pop(tick)) received.push_back(tick);

    ASSERT_GE(received.size(), 2U)
        << "PostgresFeedReader must publish both pre-inserted rows within 3s";

    for (const auto& t : received) {
        EXPECT_GT(t.price, 0.0);
        EXPECT_GT(t.volume, 0.0);
        EXPECT_EQ(t.side, 2U) << "PostgresFeedReader must set side=2 (trade)";
        EXPECT_GT(t.timestamp_ns, 0LL);
        const std::string_view sym(t.symbol.data(),
                                    ::strnlen(t.symbol.data(), t.symbol.size()));
        EXPECT_EQ(sym, kTestTicker);
    }

    // Both specific prices must appear.
    auto has_price = [&](double target) {
        return std::any_of(received.begin(), received.end(),
                           [target](const MarketTick& m) {
                               return std::abs(m.price - target) < 0.01;
                           });
    };
    EXPECT_TRUE(has_price(123.45)) << "First test row (price=123.45) must be published";
    EXPECT_TRUE(has_price(234.56)) << "Second test row (price=234.56) must be published";

    EXPECT_GE(reader.ticks_published(), 2U);
}

// Verifies the watermark: rows already published are NOT re-published on subsequent polls.
TEST_F(PostgresFeedReaderBridgeTest, WatermarkPreventsRepublishingAlreadySeenRows) {
    {
        pqxx::connection conn(conn_str());
        // Two rows in the past — will be read on the first poll.
        insert_price_row(conn, 10.0, 100.0, "NOW() - INTERVAL '20 seconds'");
        insert_price_row(conn, 20.0, 100.0, "NOW() - INTERVAL '15 seconds'");
    }

    TickRingBuffer16K output;
    FeedConfig cfg;
    cfg.symbols = {kTestTicker};

    PostgresFeedReader reader(cfg, conn_str(), output);
    reader.start();

    // Wait for first poll to publish the 2 initial rows (first poll is immediate).
    const auto first_poll_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    size_t first_count = 0;
    MarketTick tick{};
    while (std::chrono::steady_clock::now() < first_poll_deadline) {
        if (output.try_pop(tick)) ++first_count;
        if (first_count >= 2) break;
        std::this_thread::yield();
    }
    ASSERT_GE(first_count, 2U) << "First poll must deliver the 2 pre-inserted rows";

    // Insert a new row AFTER the first poll has seen the initial rows.
    {
        pqxx::connection conn(conn_str());
        insert_price_row(conn, 30.0, 100.0);  // RecordedAt = NOW()
    }

    // Wait for the second poll (reader polls every 2s).
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));

    // Drain everything that arrived after the first poll.
    std::vector<double> post_first_poll_prices;
    while (output.try_pop(tick)) post_first_poll_prices.push_back(tick.price);

    reader.stop();

    // Only the new row (price=30.0) should appear — NOT the original 2 rows again.
    EXPECT_EQ(post_first_poll_prices.size(), 1U)
        << "Watermark must prevent re-publishing the 2 rows already seen on first poll";
    if (!post_first_poll_prices.empty()) {
        EXPECT_NEAR(post_first_poll_prices[0], 30.0, 0.01)
            << "Second poll must deliver exactly the newly inserted row";
    }
}
