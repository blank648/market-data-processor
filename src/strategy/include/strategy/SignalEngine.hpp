#pragma once

#include "core/ThreadBase.hpp"
#include "core/RingBuffer.hpp"
#include "core/MarketSnapshot.hpp"
#include "strategy/Signal.hpp"

#include <unordered_map>
#include <string>

namespace mdp {

using SnapshotRingBuffer4K = RingBuffer<MarketSnapshot, 4096>;
using SignalRingBuffer4K = RingBuffer<Signal, 4096>;

class SignalEngine : public ThreadBase<SignalEngine> {
public:
    SignalEngine(SnapshotRingBuffer4K& input_queue, SignalRingBuffer4K& output_queue, int period = 20);
    ~SignalEngine() override = default;

    // Delete copy and move operations
    SignalEngine(const SignalEngine&) = delete;
    SignalEngine& operator=(const SignalEngine&) = delete;
    SignalEngine(SignalEngine&&) = delete;
    SignalEngine& operator=(SignalEngine&&) = delete;

    void run(StopToken stop_token);

private:
    SnapshotRingBuffer4K& in_queue_;
    SignalRingBuffer4K& out_queue_;
    
    int period_;
    double alpha_;
    
    // Tracks the current EMA per symbol
    std::unordered_map<std::string, double> ema_map_;
};

} // namespace mdp
