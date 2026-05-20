#include <gtest/gtest.h>
#include "strategy/SignalEngine.hpp"
#include "core/MarketSnapshot.hpp"
#include <thread>
#include <chrono>

using namespace mdp;
using namespace std::chrono_literals;

class SignalEngineTest : public ::testing::Test {
protected:
    [[nodiscard]] SnapshotRingBuffer4K& get_in_queue() noexcept { return in_queue_; }
    [[nodiscard]] SignalRingBuffer4K& get_out_queue() noexcept { return out_queue_; }

    void SetUp() override {
        engine_.start();
    }

    void TearDown() override {
        engine_.stop();
    }

private:
    SnapshotRingBuffer4K in_queue_;
    SignalRingBuffer4K out_queue_;
    
    // Default 20-period
    SignalEngine engine_{in_queue_, out_queue_, 20};
};

TEST_F(SignalEngineTest, SignalIsExactSize) {
    EXPECT_EQ(sizeof(Signal), 64);
}

TEST_F(SignalEngineTest, FirstTickInitializesEma) {
    MarketSnapshot snap{};
    snap.price = 100.0;
    std::string sym = "AAPL";
    std::copy(sym.begin(), sym.end(), snap.symbol.begin());
    
    EXPECT_TRUE(get_in_queue().try_push(snap));

    Signal sig{};
    // Wait up to 1 second for the engine to process the tick
    auto start = std::chrono::steady_clock::now();
    bool received = false;
    while (std::chrono::steady_clock::now() - start < 1s) {
        if (get_out_queue().try_pop(sig)) {
            received = true;
            break;
        }
        std::this_thread::yield();
    }

    ASSERT_TRUE(received);
    EXPECT_EQ(sig.symbol_view(), "AAPL");
    EXPECT_DOUBLE_EQ(sig.price, 100.0);
    EXPECT_DOUBLE_EQ(sig.ema_value, 100.0);
    EXPECT_EQ(sig.type, SignalType::HOLD); // Margin is 0.1%, 100 == 100
}

TEST_F(SignalEngineTest, EmaMathAndTradingRules) {
    std::string sym = "MSFT";
    
    // Tick 1
    MarketSnapshot snap1{};
    snap1.price = 100.0;
    std::copy(sym.begin(), sym.end(), snap1.symbol.begin());
    get_in_queue().try_push(snap1);

    Signal sig{};
    while (!get_out_queue().try_pop(sig)) {
        std::this_thread::yield();
    }
    
    EXPECT_DOUBLE_EQ(sig.ema_value, 100.0);
    EXPECT_EQ(sig.type, SignalType::HOLD);
    
    // Tick 2: Price jumps to 110.0
    // Expected EMA = (110.0 * alpha) + (100.0 * (1 - alpha)) 
    //              = 10.47619 + 90.47619 = 100.95238
    MarketSnapshot snap2{};
    snap2.price = 110.0;
    std::copy(sym.begin(), sym.end(), snap2.symbol.begin());
    get_in_queue().try_push(snap2);

    while (!get_out_queue().try_pop(sig)) {
        std::this_thread::yield();
    }
    
    EXPECT_NEAR(sig.ema_value, 100.95238095238095, 0.0001);
    
    // Price (110) > EMA (100.95). 
    // current_ema < current_price * 0.999 
    // 100.95 < 110 * 0.999 (109.89) -> TRUE -> BUY!
    EXPECT_EQ(sig.type, SignalType::BUY);
    
    // Tick 3: Price drops to 90.0
    // Expected EMA = (90.0 * alpha) + (100.95238095238095 * (1 - alpha))
    //              = 8.571428 + 91.337868 = 99.909297
    MarketSnapshot snap3{};
    snap3.price = 90.0;
    std::copy(sym.begin(), sym.end(), snap3.symbol.begin());
    get_in_queue().try_push(snap3);

    while (!get_out_queue().try_pop(sig)) {
        std::this_thread::yield();
    }
    
    EXPECT_NEAR(sig.ema_value, 99.9092970521542, 0.0001);
    
    // Price (90) < EMA (99.95)
    // current_ema > current_price * 1.001
    // 99.909 > 90 * 1.001 (90.09) -> TRUE -> SELL!
    EXPECT_EQ(sig.type, SignalType::SELL);
}
