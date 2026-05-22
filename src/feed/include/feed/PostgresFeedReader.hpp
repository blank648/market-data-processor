#pragma once

#include <atomic>
#include <string>

#include "core/RingBuffer.hpp"
#include "core/ThreadBase.hpp"
#include "feed/FeedConfig.hpp"
#include "feed/IFeedSource.hpp"

namespace mdp {

/**
 * @class PostgresFeedReader
 * @brief Polls the MarketPrices table for real prices written by the C# ingestion
 *        worker (Source=1, AlphaVantage) and pushes them into the tick pipeline.
 *
 * Replaces FeedSimulator's random-number generation with real market data.
 * Queries rows with RecordedAt > last_seen_epoch to avoid re-publishing ticks.
 */
class PostgresFeedReader final : public ThreadBase<PostgresFeedReader>, public IFeedSource {
    friend class ThreadBase<PostgresFeedReader>;

public:
    /**
     * @brief Constructs a PostgresFeedReader.
     * @param config     Feed configuration (symbol list used as DB filter).
     * @param conn_string libpqxx connection string.
     * @param output     Non-owning reference to the downstream ring buffer.
     */
    explicit PostgresFeedReader(FeedConfig config, std::string conn_string,
                                TickRingBuffer16K& output);

    ~PostgresFeedReader() override { stop(); }

    PostgresFeedReader(const PostgresFeedReader&) = delete;
    PostgresFeedReader& operator=(const PostgresFeedReader&) = delete;
    PostgresFeedReader(PostgresFeedReader&&) = delete;
    PostgresFeedReader& operator=(PostgresFeedReader&&) = delete;

    void start() override { ThreadBase::start(); }
    void stop()  override { ThreadBase::stop(); }

    [[nodiscard]] bool is_running() const noexcept override { return ThreadBase::is_running(); }

    [[nodiscard]] std::string_view source_name() const noexcept override {
        return "PostgresFeedReader";
    }

    uint64_t ticks_published() const noexcept override;
    uint64_t ticks_dropped()   const noexcept override;

private:
    void run(StopToken stop_token);

    FeedConfig        config_;
    std::string       conn_string_;
    TickRingBuffer16K& output_;

    std::atomic<uint64_t> ticks_published_{0};
    std::atomic<uint64_t> ticks_dropped_{0};
};

}  // namespace mdp
