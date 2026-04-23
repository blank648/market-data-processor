#include <iostream>
#include <iomanip>
#include <thread>
#include <chrono>
#include <vector>
#include <string>

#include "feed/FeedSimulator.hpp"
#include "processing/TickParser.hpp"
#include "processing/Normalizer.hpp"
#include "book/BookProcessor.hpp"
#include "core/RingBuffer.hpp"
#include "infra/Logger.hpp"
#include "infra/MetricsCollector.hpp"

#include <spdlog/spdlog.h>

using namespace mdp;

int main(int argc, char* argv[]) {
    bool json_mode = (argc > 1 && std::string(argv[1]) == "--json");

    // Pipeline setup
    TickRingBuffer16K sim_to_parser;
    TickRingBuffer4K  parser_to_norm;
    TickRingBuffer4K  norm_output;

    FeedConfig config = FeedConfig::default_config();
    config.tick_rate_hz = 10'000;
    config.symbols      = {"AAPL", "MSFT", "BTCUSD"};

    FeedSimulator sim(config, sim_to_parser);
    TickParser    parser(sim_to_parser, parser_to_norm);
    Normalizer    norm(parser_to_norm, norm_output);
    BookProcessor book(norm_output);

    Logger::init("mdp-health", spdlog::level::warn);

    // Start pipeline
    book.start();
    norm.start();
    parser.start();
    sim.start();

    // Run for exactly 500ms
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Stop pipeline and drain
    sim.stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    parser.stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    norm.stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    book.stop();

    MetricsCollector collector(sim, parser, norm, book);
    auto snap = collector.snapshot();

    // Health Checks
    std::vector<std::string> failures;
    if (snap.feed_ticks_published == 0) failures.push_back("feed_ticks_published == 0 (pipeline did not start)");
    if (snap.parser_ticks_processed == 0) failures.push_back("parser_ticks_processed == 0");
    if (snap.norm_ticks_forwarded == 0) failures.push_back("norm_ticks_forwarded == 0");
    if (snap.book_ticks_processed == 0) failures.push_back("book_ticks_processed == 0");

    double drop_rate = snap.feed_drop_rate();
    if (drop_rate >= 0.05) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "feed_drop_rate %.2f%% exceeds threshold 5.00%%", drop_rate * 100.0);
        failures.push_back(buf);
    }

    double reject_rate = snap.parser_reject_rate();
    if (reject_rate >= 0.10) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "parser_reject_rate %.2f%% exceeds threshold 10.00%%", reject_rate * 100.0);
        failures.push_back(buf);
    }

    bool healthy = failures.empty();

    // Opreste logging COMPLET
    Logger::shutdown();
    spdlog::shutdown();

    if (json_mode) {
        // Mod JSON — stdout pur, garantat curat
        std::cout << collector.to_json().dump(2) << "\n";
        std::cout.flush();
    } else {
        // STDOUT Report
        std::cout << "  ┌─────────────────────────────────────────┐\n";
        std::cout << "  │         MDP Health Check Report         │\n";
        std::cout << "  └─────────────────────────────────────────┘\n\n";

        std::cout << "\033[36m[FEED]\033[0m\n";
        std::cout << "    ticks_published : " << snap.feed_ticks_published << "\n";
        std::cout << "    ticks_dropped   : " << snap.feed_ticks_dropped << "\n";
        std::cout << "    drop_rate       : " << std::fixed << std::setprecision(2) << snap.feed_drop_rate() * 100.0 << "%\n\n";

        std::cout << "\033[36m[PARSER]\033[0m\n";
        std::cout << "    ticks_processed : " << snap.parser_ticks_processed << "\n";
        std::cout << "    ticks_rejected  : " << snap.parser_ticks_rejected << "\n";
        std::cout << "    reject_rate     : " << std::fixed << std::setprecision(2) << snap.parser_reject_rate() * 100.0 << "%\n\n";

        std::cout << "\033[36m[NORMALIZER]\033[0m\n";
        std::cout << "    ticks_forwarded    : " << snap.norm_ticks_forwarded << "\n";
        std::cout << "    ticks_deduplicated : " << snap.norm_ticks_deduplicated << "\n";
        std::cout << "    ticks_reordered    : " << snap.norm_ticks_reordered << "\n";
        std::cout << "    dedup_rate         : " << std::fixed << std::setprecision(2) << snap.norm_dedup_rate() * 100.0 << "%\n\n";

        std::cout << "\033[36m[BOOK]\033[0m\n";
        std::cout << "    ticks_processed : " << snap.book_ticks_processed << "\n";
        std::cout << "    books_active    : " << snap.book_books_active << "\n\n";

        std::cout << "[STATUS]  ";
        if (healthy) {
            std::cout << "\033[32mHEALTHY\033[0m\n";
        } else {
            std::cout << "\033[31mUNHEALTHY\033[0m\n";
            for (const auto& f : failures) {
                std::cout << "  FAIL: " << f << "\n";
            }
        }
        std::cout.flush();

        // JSON pe stderr (best-effort, pentru backwards compat)
        std::cerr << collector.to_json().dump(2) << "\n";
        std::cerr.flush();
    }

    return healthy ? 0 : 1;
}
