#include "strategy/SignalEngine.hpp"
#include <spdlog/spdlog.h>
#include <thread>
#include <cstring>

namespace mdp {

SignalEngine::SignalEngine(SnapshotRingBuffer4K& input_queue, SignalRingBuffer4K& output_queue, int period)
    : ThreadBase<SignalEngine>("SignalEngine"),
      in_queue_(input_queue),
      out_queue_(output_queue),
      period_(period)
{
    if (period_ <= 0) {
        period_ = 20;
    }
    alpha_ = 2.0 / (period_ + 1);
}

void SignalEngine::run(StopToken stop_token) {
    auto logger = spdlog::default_logger();
    if (logger) {
        logger->info("SignalEngine thread started. EMA Period: {}", period_);
    }
    
    MarketSnapshot snap;
    while (!stop_token.stop_requested()) {
        if (in_queue_.try_pop(snap)) {
            std::string sym(snap.symbol.data(), ::strnlen(snap.symbol.data(), snap.symbol.size()));
            
            double current_price = snap.price;
            double current_ema = 0.0;
            
            auto iter = ema_map_.find(sym);
            if (iter == ema_map_.end()) {
                current_ema = current_price;
                ema_map_[sym] = current_ema;
            } else {
                current_ema = (current_price * alpha_) + (iter->second * (1.0 - alpha_));
                iter->second = current_ema;
            }
            
            SignalType type = SignalType::HOLD;
            
            // Trading Rules (0.1% margin threshold)
            if (current_ema < current_price * 0.999) {
                type = SignalType::BUY;
            } else if (current_ema > current_price * 1.001) {
                type = SignalType::SELL;
            }
            
            Signal sig;
            sig.symbol = snap.symbol;
            sig.price = current_price;
            sig.ema_value = current_ema;
            sig.timestamp_ns = snap.timestamp_ns;
            sig.type = type;
            
            // Non-blocking forward push
            while (!out_queue_.try_push(sig) && !stop_token.stop_requested()) {
                std::this_thread::yield();
            }
        } else {
            // Nothing to process, yield to avoid 100% CPU on empty queue
            std::this_thread::yield();
        }
    }
    
    if (logger) {
        logger->info("SignalEngine thread stopped.");
    }
}

} // namespace mdp
