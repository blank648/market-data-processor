#pragma once

#include <atomic>
#include <chrono>
#include <random>
#include <string_view>
#include <vector>

#include "core/MarketTick.hpp"
#include "core/RingBuffer.hpp"
#include "core/ThreadBase.hpp"
#include "feed/FeedConfig.hpp"
#include "feed/IFeedSource.hpp"

namespace mdp {

/**
 * @class FeedSimulator
 * @brief Simulates a live market data feed by generating random ticks.
 *
 * Implements IFeedSource and utilizes a thread (ThreadBase) to asynchronously
 * produce market ticks and push them into an output lock-free SPSC ring buffer.
 */
class FeedSimulator final : public ThreadBase, public IFeedSource {
public:
    /**
     * @brief Constructs a new FeedSimulator.
     * @param config The feed configuration (symbols, rate, volatility).
     * @param output Non-owning reference to the output TickRingBuffer.
     */
    explicit FeedSimulator(FeedConfig config, TickRingBuffer16K& output);

    ~FeedSimulator() override { stop(); }

    // Explicit delegation resolves name collision between
    // ThreadBase (implementation) and IFeedSource (interface).
    // Both bases declare start/stop/is_running — ThreadBase wins.

    /**
     * @brief Starts the underlying simulation thread.
     */
    void start() override {
        ThreadBase::start();   // explicitly delegates to jthread creation
    }

    /**
     * @brief Stops the underlying simulation thread gracefully.
     */
    void stop() override {
        ThreadBase::stop();    // explicitly delegates to request_stop + join
    }

    /**
     * @brief Checks whether the simulation thread is currently running.
     * @return true if the thread is running, false otherwise.
     */
    [[nodiscard]] bool is_running() const noexcept override {
        return ThreadBase::is_running();  // explicitly delegates to atomic flag
    }

    /**
     * @brief Gets the name of the simulated feed source.
     * @return std::string_view representing the feed's name.
     */
    [[nodiscard]] std::string_view source_name() const noexcept override {
        return "FeedSimulator";
    }

    /**
     * @brief Retrieves the number of successfully published ticks.
     * @return uint64_t count of published ticks.
     */
    uint64_t ticks_published() const noexcept override;

    /**
     * @brief Retrieves the number of ticks dropped due to a full buffer.
     * @return uint64_t count of dropped ticks.
     */
    uint64_t ticks_dropped() const noexcept override;

private:
    /**
     * @brief The thread function generating ticks loop.
     * @param st A StopToken provided by ThreadBase to signal stopping.
     */
    void run(StopToken st) override;

    /**
     * @brief Generates a tick for the given symbol index.
     * @param symbol_idx The index into the config's symbol list.
     * @return MarketTick The simulated tick.
     */
    // [THREAD OWNERSHIP] rng_ and price_noise_ are accessed EXCLUSIVELY
    // from run() which executes on the jthread. They are initialized in
    // the constructor (before start() is called) and never touched from
    // any other thread. No synchronization required.
    // WARNING: Do NOT access rng_ or price_noise_ from test threads or
    // any thread other than the jthread created by start().
    MarketTick generate_tick(std::size_t symbol_idx) noexcept;

    /**
     * @brief Controls the loop rate to match the configured tick rate.
     * @param next_tick The target time point for the next wake-up.
     * @param interval The computed time interval between ticks.
     */
    // [RATE CONTROL] Guard = min(50µs, interval/10).
    // Prevents guard > interval at high frequencies (>100kHz).
    // Below 20kHz: 50µs guard gives precise spin-to-deadline.
    // Above 100kHz: guard = interval/10 preserves sleep benefit.
    void rate_control(std::chrono::steady_clock::time_point& next_tick,
                      std::chrono::nanoseconds interval) noexcept;

    FeedConfig config_;
    TickRingBuffer16K& output_;
    std::vector<double> current_prices_;  // mutable random walk state

    // RNG — Mersenne Twister, seeded with random_device
    std::mt19937 rng_;
    std::normal_distribution<double> price_noise_;  // mean=0, stddev=volatility

    // Counters (atomics — readable from outside without stopping)
    std::atomic<uint64_t> ticks_published_{0};
    std::atomic<uint64_t> ticks_dropped_{0};
};

} // namespace mdp
