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
    static void init(std::string_view app_name, spdlog::level::level_enum level = spdlog::level::info) {
        // [SAFETY] Leak-on-exit pattern: ensures the logger object outlives all other statics.
        // This prevents segfaults during static destruction.
        static spdlog::logger* leaked_logger = nullptr;

        if (leaked_logger == nullptr) {
            try {
                auto logger = spdlog::stdout_color_mt(std::string(app_name));
                // Intentional leak of the shared_ptr to keep the logger alive forever
                auto* persistent = new std::shared_ptr<spdlog::logger>(logger); // NOLINT(cppcoreguidelines-owning-memory)
                leaked_logger = persistent->get();
            } catch (const spdlog::spdlog_ex&) {
                auto existing = spdlog::get(std::string(app_name));
                if (existing) {
                    static auto* persistent = new std::shared_ptr<spdlog::logger>(existing); // NOLINT(cppcoreguidelines-owning-memory)
                    leaked_logger = persistent->get();
                }
            }
        }

        if (leaked_logger != nullptr) {
            // Re-register as default in case spdlog::shutdown() was called previously
            spdlog::set_default_logger(std::shared_ptr<spdlog::logger>(leaked_logger, [](spdlog::logger*){}));
            leaked_logger->set_level(level);
            leaked_logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%^%l%$] [thread %t] %v");
            spdlog::set_level(level);
        }
    }

    // Get a component-scoped named logger.
    static std::shared_ptr<spdlog::logger> get(std::string_view name) {
        try {
            auto logger = spdlog::get(std::string(name));
            if (logger) {
                return logger;
            }

            auto default_logger = spdlog::default_logger();
            if (default_logger) {
                logger = default_logger->clone(std::string(name));
                logger->set_level(default_logger->level());
                spdlog::register_logger(logger);
                return logger;
            }
        } catch (...) {
            // If spdlog is already partially destroyed, return a dummy or null
        }
        
        // Fallback: Create an unregistered logger to avoid registry issues during teardown
        auto sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        auto fallback = std::make_shared<spdlog::logger>(std::string(name), sink);
        fallback->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%^%l%$] [thread %t] %v");
        fallback->set_level(spdlog::level::info);
        return fallback;
    }

    // Shutdown spdlog (flush queues, drop loggers)
    [[maybe_unused]] static void shutdown() {
        spdlog::shutdown();
    }
};

} // namespace mdp
