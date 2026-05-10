// Copyright (c) 2026 Market Data Processor Project. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <string_view>
#include <memory>

// Convenience macros for global logging (main/tests)
#define LOG_TRACE(...)    SPDLOG_TRACE(__VA_ARGS__)
#define LOG_DEBUG(...)    SPDLOG_DEBUG(__VA_ARGS__)
#define LOG_INFO(...)     SPDLOG_INFO(__VA_ARGS__)
#define LOG_WARN(...)     SPDLOG_WARN(__VA_ARGS__)
#define LOG_ERROR(...)    SPDLOG_ERROR(__VA_ARGS__)
#define LOG_CRITICAL(...) SPDLOG_CRITICAL(__VA_ARGS__)

namespace mdp {

class Logger {
public:
    // Initialize the global logger instance.
    // Thread safety: spdlog::stdout_color_mt is multi-threaded safe.
    static void init(std::string_view app_name, spdlog::level::level_enum level = spdlog::level::info) {
        auto logger = spdlog::stdout_color_mt(std::string(app_name));
        logger->set_level(level);
        logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%^%l%$] [thread %t] %v");
        spdlog::set_default_logger(logger);
        spdlog::set_level(level);
    }

    // Get a component-scoped named logger.
    static std::shared_ptr<spdlog::logger> get(std::string_view name) {
        auto logger = spdlog::get(std::string(name));
        if (!logger) {
            auto default_logger = spdlog::default_logger();
            if (default_logger) {
                logger = default_logger->clone(std::string(name));
                // Inherit level from the default logger directly — avoids
                // spdlog::get_level() which dereferences default_logger_raw()
                // and crashes if no default logger has been registered yet.
                logger->set_level(default_logger->level());
            } else {
                // Use raw sink to avoid spdlog::stdout_color_mt(), which both
                // creates AND auto-registers — causing spdlog::register_logger()
                // below to throw if the name was re-used after spdlog::shutdown().
                auto sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
                logger = std::make_shared<spdlog::logger>(std::string(name), sink);
                logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%^%l%$] [thread %t] %v");
                logger->set_level(spdlog::level::info);
            }
            spdlog::register_logger(logger);
        }
        return logger;
    }

    // Shutdown spdlog (flush queues, drop loggers)
    [[maybe_unused]] static void shutdown() {
        spdlog::shutdown();
    }
};

} // namespace mdp
