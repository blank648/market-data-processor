#include "feed/FeedSimulator.hpp"
#include "infra/Logger.hpp"
#include <algorithm>
#include <thread>

namespace mdp {

FeedSimulator::FeedSimulator(FeedConfig config, TickRingBuffer16K& output)
    : ThreadBase("FeedSimulator"),
      config_(std::move(config)),
      output_(output),
      current_prices_(config_.initial_prices),
      rng_(std::random_device{}()),
      price_noise_(0.0, config_.volatility) {
}



uint64_t FeedSimulator::ticks_published() const noexcept {
    return ticks_published_.load(std::memory_order_relaxed);
}

uint64_t FeedSimulator::ticks_dropped() const noexcept {
    return ticks_dropped_.load(std::memory_order_relaxed);
}

void FeedSimulator::run(StopToken st) {
    auto log_ = mdp::Logger::get("FeedSimulator");
    
    if (config_.tick_rate_hz == 0 || config_.symbols.empty()) {
        log_->info("FeedSimulator stopped, ticks_generated={}", ticks_published_.load(std::memory_order_relaxed));
        return;
    }

    log_->info("FeedSimulator starting, interval={}ms", 1000.0 / config_.tick_rate_hz);

    auto interval = std::chrono::nanoseconds(1'000'000'000ULL / config_.tick_rate_hz);
    auto next_tick = std::chrono::steady_clock::now();

    while (!st.stop_requested()) {
        for (std::size_t symbol_idx = 0; symbol_idx < config_.symbols.size(); ++symbol_idx) {
            auto tick = generate_tick(symbol_idx);
            log_->trace("Tick generated: {} @ {:.4f}", tick.symbol.data(), tick.price);
            if (output_.try_push(std::move(tick))) {
                ticks_published_.fetch_add(1, std::memory_order_relaxed);
            } else {
                ticks_dropped_.fetch_add(1, std::memory_order_relaxed);
            }
        }
        
        rate_control(next_tick, interval);
    }
    
    log_->info("FeedSimulator stopped, ticks_generated={}", ticks_published_.load(std::memory_order_relaxed));
}

// [THREAD OWNERSHIP] rng_ and price_noise_ are accessed EXCLUSIVELY
// from run() which executes on the jthread. They are initialized in
// the constructor (before start() is called) and never touched from
// any other thread. No synchronization required.
// WARNING: Do NOT access rng_ or price_noise_ from test threads or
// any thread other than the jthread created by start().
MarketTick FeedSimulator::generate_tick(std::size_t symbol_idx) noexcept {
    // [RANDOM WALK]
    // Uses a Gaussian multiplicative model. The price is multiplied by (1.0 + Noise)
    // where Noise is drawn from a normal distribution with mean 0 and stddev = volatility.
    // This realistic random walk mimics the log-normal price movement in financial markets.
    current_prices_[symbol_idx] *= (1.0 + price_noise_(rng_));
    
    // Clamp price to > 0 (avoid negative prices)
    current_prices_[symbol_idx] = std::clamp(current_prices_[symbol_idx], 0.01, 1'000'000'000.0);
    
    // Generate volume: uniform_real_distribution [0.01, 10.0]
    std::uniform_real_distribution<double> vol_dist(0.01, 10.0);
    double volume = vol_dist(rng_);
    
    // Alternate side: bid/ask per tick (use ticks_published_ % 2 == 0)
    std::uint8_t side = (ticks_published_.load(std::memory_order_relaxed) % 2 == 0) ? 0 : 1;
    
    return MarketTick::make(config_.symbols[symbol_idx], current_prices_[symbol_idx], volume, side);
}

void FeedSimulator::rate_control(std::chrono::steady_clock::time_point& next_tick,
                                 std::chrono::nanoseconds interval) noexcept {
    next_tick += interval;
    
    // [RATE CONTROL] Guard = min(50µs, interval/10).
    // Prevents guard > interval at high frequencies (>100kHz).
    // Below 20kHz: 50µs guard gives precise spin-to-deadline.
    // Above 100kHz: guard = interval/10 preserves sleep benefit.
    auto guard = std::min(
        std::chrono::microseconds(50),
        std::chrono::duration_cast<std::chrono::microseconds>(interval) / 10
    );
    auto sleep_target = next_tick - guard;
    
    if (std::chrono::steady_clock::now() < sleep_target) {
        std::this_thread::sleep_until(sleep_target);
    }
    
    while (std::chrono::steady_clock::now() < next_tick) {
        std::this_thread::yield();
    }
}

} // namespace mdp
