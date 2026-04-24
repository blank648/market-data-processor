#include <gtest/gtest.h>
#include <thread>
#include <chrono>

#include "feed/FeedSimulator.hpp"
#include "processing/TickParser.hpp"
#include "processing/Normalizer.hpp"
#include "book/BookProcessor.hpp"
#include "core/RingBuffer.hpp"
#include "infra/Logger.hpp"
#include "infra/MetricsCollector.hpp"

using namespace mdp;

class MetricsCollectorTest : public ::testing::Test {
protected:
    void SetUp() override {
        Logger::init("metrics-test", spdlog::level::warn);
    }
    void TearDown() override {
        Logger::shutdown();
    }
};

TEST_F(MetricsCollectorTest, SnapshotCapturesNonZeroCounts) {
    TickRingBuffer16K sim_to_parser;
    TickRingBuffer4K  parser_to_norm;
    TickRingBuffer4K  norm_output;

    FeedConfig config = FeedConfig::default_config();
    config.tick_rate_hz = 5'000;
    config.symbols = {"AAPL", "MSFT"};
    config.initial_prices = {150.0, 300.0};

    FeedSimulator sim(config, sim_to_parser);
    TickParser    parser(sim_to_parser, parser_to_norm);
    Normalizer    norm(parser_to_norm, norm_output);
    BookProcessor book(norm_output);

    book.start();
    norm.start();
    parser.start();
    sim.start();

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    auto drain_buffer = [](auto& buf,
                           std::chrono::milliseconds timeout = std::chrono::milliseconds(200)) {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!buf.empty() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    };

    sim.stop();
    drain_buffer(sim_to_parser);
    parser.stop();
    drain_buffer(parser_to_norm);
    norm.stop();
    drain_buffer(norm_output);
    book.stop();

    MetricsCollector collector(sim, parser, norm, book);
    auto snapshot = collector.snapshot();

    EXPECT_GT(snapshot.feed_ticks_published, 0);
    EXPECT_GT(snapshot.parser_ticks_processed, 0);
    EXPECT_GT(snapshot.norm_ticks_forwarded, 0);
    EXPECT_GT(snapshot.book_ticks_processed, 0);
    EXPECT_GT(snapshot.snapshot_time_ns, 0);
}

TEST_F(MetricsCollectorTest, DropRateIsZeroOrPositive) {
    TickRingBuffer16K sim_to_parser;
    TickRingBuffer4K  parser_to_norm;
    TickRingBuffer4K  norm_output;

    FeedConfig config = FeedConfig::default_config();
    config.tick_rate_hz = 5'000;

    FeedSimulator sim(config, sim_to_parser);
    TickParser    parser(sim_to_parser, parser_to_norm);
    Normalizer    norm(parser_to_norm, norm_output);
    BookProcessor book(norm_output);

    book.start();
    norm.start();
    parser.start();
    sim.start();

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    sim.stop();
    parser.stop();
    norm.stop();
    book.stop();

    MetricsCollector collector(sim, parser, norm, book);
    auto snapshot = collector.snapshot();

    EXPECT_GE(snapshot.feed_drop_rate(), 0.0);
    EXPECT_LE(snapshot.feed_drop_rate(), 1.0);
    EXPECT_GE(snapshot.parser_reject_rate(), 0.0);
}

TEST_F(MetricsCollectorTest, ToJsonProducesValidStructure) {
    TickRingBuffer16K sim_to_parser;
    TickRingBuffer4K  parser_to_norm;
    TickRingBuffer4K  norm_output;

    FeedConfig config = FeedConfig::default_config();
    config.tick_rate_hz = 1'000;

    FeedSimulator sim(config, sim_to_parser);
    TickParser    parser(sim_to_parser, parser_to_norm);
    Normalizer    norm(parser_to_norm, norm_output);
    BookProcessor book(norm_output);

    book.start();
    norm.start();
    parser.start();
    sim.start();

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    sim.stop();
    parser.stop();
    norm.stop();
    book.stop();

    MetricsCollector collector(sim, parser, norm, book);
    auto json = collector.to_json();

    EXPECT_TRUE(json.contains("feed"));
    if (json.contains("feed")) {
        EXPECT_TRUE(json["feed"].contains("ticks_published"));
    }
    EXPECT_TRUE(json.contains("derived"));
    if (json.contains("derived")) {
        EXPECT_TRUE(json["derived"]["feed_drop_rate"].is_number());
    }
    EXPECT_TRUE(json["snapshot_time_ns"].is_number_integer());
}

TEST_F(MetricsCollectorTest, ToLogDoesNotThrow) {
    TickRingBuffer16K sim_to_parser;
    TickRingBuffer4K  parser_to_norm;
    TickRingBuffer4K  norm_output;

    FeedConfig config = FeedConfig::default_config();
    config.tick_rate_hz = 1'000;

    FeedSimulator sim(config, sim_to_parser);
    TickParser    parser(sim_to_parser, parser_to_norm);
    Normalizer    norm(parser_to_norm, norm_output);
    BookProcessor book(norm_output);

    book.start();
    norm.start();
    parser.start();
    sim.start();

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    sim.stop();
    parser.stop();
    norm.stop();
    book.stop();

    MetricsCollector collector(sim, parser, norm, book);
    ASSERT_NO_THROW(collector.to_log());
}
