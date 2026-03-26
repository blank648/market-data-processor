// Copyright (c) 2026 Market Data Processor Project. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <atomic>
#include <stdexcept>
#include <string>
#include <thread>

namespace mdp {

/// @brief A lightweight stop token polyfill for environments lacking C++20 <stop_token>.
class StopToken {
public:
    explicit StopToken(const std::atomic<bool>* flag) noexcept : flag_(flag) {}

    [[nodiscard]] bool stop_requested() const noexcept {
        return flag_ ? flag_->load(std::memory_order_relaxed) : false;
    }

private:
    const std::atomic<bool>* flag_;
};

/// @brief Abstract RAII base class for named pipeline-stage threads.
///
/// ThreadBase owns a `std::thread` and manages its full lifecycle:
/// construction, startup, cooperative cancellation via an atomic flag,
/// and deterministic teardown on destruction. Subclasses override the
/// protected `run()` method to implement their processing loop.
///
/// Thread safety:
/// - `start()` and `stop()` must be called from the **same** external thread.
/// - `is_running()` is safe to query from any thread.
///
/// @par Example
/// @code
///     class TickPrinter : public mdp::ThreadBase {
///     public:
///         explicit TickPrinter() : mdp::ThreadBase("TickPrinter") {}
///     protected:
///         void run(mdp::StopToken st) override {
///             while (!st.stop_requested()) {
///                 // process ticks …
///             }
///         }
///     };
///
///     TickPrinter printer;
///     printer.start();
///     // … do work …
///     printer.stop();   // or just let it go out of scope
/// @endcode
class ThreadBase {
public:
    // ─── Construction / Destruction ───────────────────────────────────────

    /// @brief Constructs a ThreadBase with the given human-readable name.
    ///
    /// The thread is **not** started automatically; call `start()` explicitly.
    ///
    /// @param name Descriptive label used in logs and diagnostics.
    explicit ThreadBase(std::string name) : name_(std::move(name)) {}

    /// @brief Destructor — calls `stop()` to ensure the thread is joined.
    virtual ~ThreadBase() { stop(); }

    // ─── Non-copyable, non-movable ────────────────────────────────────────

    ThreadBase(const ThreadBase&)            = delete;
    ThreadBase& operator=(const ThreadBase&) = delete;
    ThreadBase(ThreadBase&&)                 = delete;
    ThreadBase& operator=(ThreadBase&&)      = delete;

    // ─── Public API ───────────────────────────────────────────────────────

    /// @brief Launches the worker thread and invokes `run()`.
    ///
    /// The `running_` flag is set to `true` before `run()` is called and
    /// reset to `false` in a finally-like RAII guard, even if `run()` throws.
    /// Exceptions thrown by `run()` are caught and silently swallowed to
    /// prevent `std::terminate`; the flag is still cleared on exit.
    ///
    /// @throws std::runtime_error if the thread is already running.
    void start() {
        if (running_.load(std::memory_order_relaxed)) {
            throw std::runtime_error("ThreadBase[" + name_ + "]: already running");
        }

        stop_flag_.store(false, std::memory_order_relaxed);

        thread_ = std::thread([this]() {
            running_.store(true, std::memory_order_relaxed);

            // RAII guard: always clear the flag when run() returns or throws.
            struct FinallyGuard {
                std::atomic<bool>& flag;
                ~FinallyGuard() { flag.store(false, std::memory_order_relaxed); }
            } guard{running_};

            try {
                run(StopToken(&stop_flag_));
            } catch (...) {
                // Swallow: prevents std::terminate; flag is cleared by guard.
            }
        });
    }

    /// @brief Cooperatively stops the worker thread and joins it.
    ///
    /// Requests a stop via the atomic flag, then blocks until the `thread_`
    /// has joined. Safe to call even if the thread was never started, or has
    /// already stopped.
    void stop() {
        stop_flag_.store(true, std::memory_order_relaxed);
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    /// @brief Returns `true` while the worker thread is executing `run()`.
    ///
    /// The flag is set **inside** the thread body, so there is a brief window
    /// between `start()` returning and the thread setting the flag. Callers
    /// must not rely on this for strict synchronisation — use it only for
    /// lightweight status checks and logging.
    [[nodiscard]] bool is_running() const noexcept {
        return running_.load(std::memory_order_relaxed);
    }

    /// @brief Returns the human-readable name assigned at construction.
    [[nodiscard]] const std::string& name() const noexcept { return name_; }

protected:
    // ─── Protected Interface ──────────────────────────────────────────────

    /// @brief Override to implement the thread's processing loop.
    ///
    /// Poll `stop_token.stop_requested()` regularly to honour cooperative
    /// cancellation. This method is called exactly once per `start()`.
    ///
    /// @param stop_token Cancellation token; check periodically to exit cleanly.
    virtual void run(StopToken stop_token) = 0;

    /// @brief The name supplied at construction; accessible to subclasses.
    const std::string name_;

private:
    std::thread       thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_flag_{false};
};

}  // namespace mdp
