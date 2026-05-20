#include "feed/FeedSimulator.hpp"
#include "infra/Logger.hpp"
#include <algorithm>
#include <thread>
#include <cassert>

namespace mdp {

namespace {

void update_price(SymbolState& state, std::mt19937_64& rng) {
    // [MID PRICE] Random walk — bounded Brownian motion
    // Sigma is proportional to price (geometric Brownian motion)
    std::normal_distribution<double> mid_walk(0.0, state.mid_price * 0.001);
    state.mid_price += mid_walk(rng);
    
    // Clamp limits — BTCUSD has much higher scale
    const double lower_bound = 10.0;
    const double upper_bound = (state.symbol == "BTCUSD") ? 1000000.0 : 10000.0;
    state.mid_price = std::clamp(state.mid_price, lower_bound, upper_bound);

    // [SPREAD] Mean-reversion toward target
    double target = 0.01;
    double s_min = 0.001;
    double s_max = 0.05;
    double noise_sigma = 0.002;

    if (state.symbol == "BTCUSD") {
        target = 5.0;
        s_min = 0.5;
        s_max = 50.0;
        noise_sigma = 1.0; // scaled noise for BTC
    }

    constexpr double REVERSION = 0.1;
    std::normal_distribution<double> spread_noise(0.0, noise_sigma);
    state.spread += (REVERSION * (target - state.spread)) + spread_noise(rng);
    state.spread = std::clamp(state.spread, s_min, s_max);

    // [SIZE] Random walk — always positive
    std::normal_distribution<double> size_walk(0.0, 10.0);
    state.bid_size = std::clamp(state.bid_size + size_walk(rng), 1.0, 10000.0);
    state.ask_size = std::clamp(state.ask_size + size_walk(rng), 1.0, 10000.0);
}

} // namespace

FeedSimulator::FeedSimulator(FeedConfig config, TickRingBuffer16K& output)
    : ThreadBase("FeedSimulator"),
      config_(std::move(config)),
      output_(output),
      rng_(std::random_device{}()) {
    
    for (std::size_t i = 0; i < config_.symbols.size(); ++i) {
        SymbolState symbol_state;
        symbol_state.symbol = config_.symbols[i];
        symbol_state.mid_price = (i < config_.initial_prices.size()) ? config_.initial_prices[i] : 100.0;
        
        // Specific initial states per symbol
        if (symbol_state.symbol == "AAPL") { symbol_state.spread = 0.02; symbol_state.bid_size = 100.0; symbol_state.ask_size = 100.0; }
        else if (symbol_state.symbol == "MSFT") { symbol_state.spread = 0.02; symbol_state.bid_size = 100.0; symbol_state.ask_size = 100.0; }
        else if (symbol_state.symbol == "BTCUSD") { symbol_state.spread = 5.0; symbol_state.bid_size = 1.0; symbol_state.ask_size = 1.0; }
        else { symbol_state.spread = 0.02; symbol_state.bid_size = 100.0; symbol_state.ask_size = 100.0; }
        
        symbol_states_.push_back(symbol_state);
    }
}

uint64_t FeedSimulator::ticks_published() const noexcept {
    return ticks_published_.load(std::memory_order_relaxed);
}

uint64_t FeedSimulator::ticks_dropped() const noexcept {
    return ticks_dropped_.load(std::memory_order_relaxed);
}

void FeedSimulator::run(StopToken stop_token) {
    auto log_ = mdp::Logger::get("FeedSimulator");
    
    if (config_.tick_rate_hz == 0 || config_.symbols.empty()) {
        log_->info("FeedSimulator stopped, ticks_generated={}", ticks_published_.load(std::memory_order_relaxed));
        return;
    }

    log_->info("FeedSimulator starting, interval={}ms", 1000.0 / static_cast<double>(config_.tick_rate_hz));

    auto interval = std::chrono::nanoseconds(1'000'000'000ULL / config_.tick_rate_hz);
    auto next_tick = std::chrono::steady_clock::now();

    while (!stop_token.stop_requested()) {
        for (std::size_t symbol_idx = 0; symbol_idx < config_.symbols.size(); ++symbol_idx) {
            auto tick = generate_tick(symbol_idx);
            
            #ifndef NDEBUG
            // Derive reconstructed bid/ask for verification from the tick if possible, 
            // but we use the state directly as per instruction pattern even if tick 
            // only has one price.
            const auto& symbol_state = symbol_states_[symbol_idx];
            double bid = symbol_state.mid_price - (symbol_state.spread / 2.0);
            double ask = symbol_state.mid_price + (symbol_state.spread / 2.0);

            assert(ask > bid &&
                   bid > 0.0 &&
                   ask > 0.0 &&
                   symbol_state.bid_size > 0.0 &&
                   symbol_state.ask_size > 0.0 &&
                   "FeedSimulator generated invalid tick");
            #endif

            log_->trace("Tick generated: {} @ {:.4f}", tick.symbol.data(), tick.price);
            if (output_.try_push(tick)) {
                ticks_published_.fetch_add(1, std::memory_order_relaxed);
            } else {
                ticks_dropped_.fetch_add(1, std::memory_order_relaxed);
            }
        }
        
        rate_control(next_tick, interval);
    }
    
    log_->info("FeedSimulator stopped, ticks_generated={}", ticks_published_.load(std::memory_order_relaxed));
}

MarketTick FeedSimulator::generate_tick(std::size_t symbol_idx) noexcept {
    auto& state = symbol_states_[symbol_idx];
    update_price(state, rng_);
    
    double bid = state.mid_price - (state.spread / 2.0);
    double ask = state.mid_price + (state.spread / 2.0);

    // Alternate side: bid/ask per tick (use ticks_published_ % 2 == 0)
    bool is_bid = (ticks_published_.load(std::memory_order_relaxed) % 2 == 0);
    double price = is_bid ? bid : ask;
    double volume = is_bid ? state.bid_size : state.ask_size;
    uint8_t side = is_bid ? 0 : 1;

    return MarketTick::make(state.symbol, price, volume, side);
}

void FeedSimulator::rate_control(std::chrono::steady_clock::time_point& next_tick,
                                 std::chrono::nanoseconds interval) noexcept {
    next_tick += interval;
    
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
