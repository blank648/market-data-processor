#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>

#include "feed/FeedSimulator.hpp"
#include "processing/TickParser.hpp"
#include "processing/Normalizer.hpp"
#include "book/BookProcessor.hpp"
#include "infra/DbWriter.hpp"
#include "infra/Logger.hpp"
#include "core/RingBuffer.hpp"

#include <spdlog/spdlog.h>

using namespace mdp;

// Global flag for signal handling
std::atomic<bool> g_running{true};

void signal_handler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        g_running = false;
    }
}

std::vector<std::string> split_symbols(const std::string& input) {
    std::vector<std::string> symbols;
    std::stringstream ss(input);
    std::string item;
    while (std::getline(ss, item, ',')) {
        if (!item.empty()) {
            symbols.push_back(item);
        }
    }
    return symbols;
}

int main() {
    // 1. Initialize Logger
    Logger::init("mdp-processor", spdlog::level::info);
    auto log = Logger::get("Main");

    log->info("Starting Market Data Processor service...");

    // 2. Read DB Connection String
    const char* db_conn_env = std::getenv("MDP_DB_CONNECTION");
    if (!db_conn_env) {
        log->error("Environment variable MDP_DB_CONNECTION is missing!");
        return 1;
    }
    std::string db_conn_str(db_conn_env);

    // 3. Read Symbols
    const char* symbols_env = std::getenv("MDP_SYMBOLS");
    std::string symbols_str = symbols_env ? symbols_env : "AAPL,MSFT,GOOGL,IBM,TSLA,AMZN";
    std::vector<std::string> symbols = split_symbols(symbols_str);
    
    log->info("Configured symbols: {}", symbols_str);

    // 4. Register Signal Handlers
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // 5. Build Pipeline Components
    
    // Ring Buffers
    TickRingBuffer16K sim_to_parser;
    TickRingBuffer4K  parser_to_norm;
    TickRingBuffer4K  norm_to_book;
    TickRingBuffer4K  book_to_db;

    // Feed Config
    FeedConfig config = FeedConfig::default_config();
    config.symbols = symbols;
    config.initial_prices.clear();
    for (size_t i = 0; i < symbols.size(); ++i) {
        config.initial_prices.push_back(100.0 + (static_cast<double>(i) * 10.0));
    }
    config.tick_rate_hz = 1000;
    
    // Stages
    log->info("Initializing pipeline stages...");
    FeedSimulator sim(config, sim_to_parser);
    TickParser    parser(sim_to_parser, parser_to_norm);
    Normalizer    norm(parser_to_norm, norm_to_book);
    BookProcessor book(norm_to_book, book_to_db);
    DbWriter      db_writer(book_to_db, db_conn_str);

    // 6. Start Stages in Order
    log->info("Starting pipeline stages...");
    db_writer.start();
    book.start();
    norm.start();
    parser.start();
    sim.start();

    log->info("Processor service is running. Press Ctrl+C to stop.");

    // 7. Wait for Signal
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    log->info("Shutdown signal received. Stopping pipeline...");

    // 8. Stop Stages in Reverse Order
    sim.stop();
    log->info("FeedSimulator stopped.");
    
    parser.stop();
    log->info("TickParser stopped.");
    
    norm.stop();
    log->info("Normalizer stopped.");
    
    book.stop();
    log->info("BookProcessor stopped.");
    
    db_writer.stop();
    log->info("DbWriter stopped.");

    log->info("Market Data Processor service stopped successfully.");

    Logger::shutdown();
    spdlog::shutdown();

    return 0;
}
