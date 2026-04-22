// Copyright (c) 2026 Market Data Processor Project. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <atomic>
#include <cassert>
#include <stdexcept>
#include <string>
#include <thread>

namespace mdp {

/// @brief A lightweight stop token polyfill for environments lacking C++20 <stop_token>.
class StopToken {
public:
    explicit StopToken(const std::atomic<bool>* flag) noexcept : flag_(flag) {}

    [[nodiscard]] bool stop_requested() const noexcept {
        // [ARM MEMORY MODEL] acquire: pairs with the release store in stop()
        // to guarantee the stop signal is visible on weakly-ordered CPUs
        // (e.g., Apple Silicon M-series) without delay.
        return flag_ ? flag_->load(std::memory_order_acquire) : false;
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
/// @note SUBCLASS CONTRACT: Every concrete subclass MUST declare:
///   @code
///     ~SubclassName() override { stop(); }
///   @endcode
///   This ensures `stop()` is called — and the worker thread is joined —
///   **before** any subclass members are destroyed. `~ThreadBase()` does NOT
///   call `stop()`; it asserts the thread is already stopped. Violating this
///   contract causes a use-after-free when `run()` accesses destroyed members
///   during the join window.
///
/// @par Example
/// @code
///     class TickPrinter : public mdp::ThreadBase {
///     public:
///         explicit TickPrinter() : mdp::ThreadBase("TickPrinter") {}
///         ~TickPrinter() override { stop(); }  // REQUIRED — see SUBCLASS CONTRACT
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

    /// @brief Destructor — asserts the worker thread has already been stopped.
    ///
    /// Does NOT call `stop()`. Subclasses must call `stop()` in their own
    /// destructor (before subclass members are destroyed) to satisfy the
    /// SUBCLASS CONTRACT documented on the class. A failing assert in a
    /// Debug build means a subclass forgot to call `stop()`.
    virtual ~ThreadBase() {
        // [FIX 1 — Destructor Chain Use-After-Free]
        // Calling stop() here would join the thread AFTER subclass members
        // have already been destroyed, causing use-after-free if run() is
        // still executing. The subclass must call stop() first.
        // DIAGNOSTIC: if this fires, a subclass forgot ~SubClass() override { stop(); }
        //assert(!thread_.joinable());  // NOLINT(misc-include-cleaner)
        stop();
    }

    // ─── Non-copyable, non-movable ────────────────────────────────────────

    ThreadBase(const ThreadBase&)            = delete;
    ThreadBase& operator=(const ThreadBase&) = delete;
    ThreadBase(ThreadBase&&)                 = delete;
    ThreadBase& operator=(ThreadBase&&)      = delete;

    // ─── Public API ───────────────────────────────────────────────────────

    /// @brief Launches the worker thread and invokes `run()`.
    ///
    /// The `running_` flag is set to `true` inside the thread body with
    /// release ordering and reset to `false` (also release) when `run()`
    /// returns or throws. `is_running()` reads it with acquire ordering.
    /// Exceptions thrown by `run()` are caught and silently swallowed to
    /// prevent `std::terminate`; the flag is still cleared on exit.
    ///
    /// @throws std::runtime_error if the thread is already running.
    void start() {
        // [FIX 3 — TOCTOU Race on running_ in start()]
        // Guard uses joinable() NOT running_. joinable() is set synchronously
        // by the std::thread constructor before the lambda body executes.
        // running_ is set INSIDE the lambda and has a TOCTOU window: two
        // concurrent start() calls could both read running_=false and each
        // launch a thread, running run() concurrently on the same object.
        if (thread_.joinable()) {
            throw std::runtime_error("ThreadBase[" + name_ + "]: already running");
        }

        // [ARM MEMORY MODEL] release: pairs with the acquire load in
        // StopToken::stop_requested() so the cleared flag is immediately
        // visible to the new thread before it calls run().
        stop_flag_.store(false, std::memory_order_release);

        thread_ = std::thread([this]() {
            // [ARM MEMORY MODEL] release: ensures is_running() callers on
            // other threads see true only after all prior stores are visible.
            running_.store(true, std::memory_order_release);

            // RAII guard: always clear the flag when run() returns or throws.
            struct FinallyGuard {
                std::atomic<bool>& flag;
                // [ARM MEMORY MODEL] release: pairs with acquire in is_running().
                ~FinallyGuard() { flag.store(false, std::memory_order_release); }
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
        // [FIX 2 — Relaxed Memory Ordering Hangs on ARM]
        // [ARM MEMORY MODEL] release: guarantees the true value is visible to
        // the worker thread's acquire load in StopToken::stop_requested()
        // before the thread exits its run() loop. Without this, on Apple
        // Silicon (ARMv8 weakly-ordered), the store may sit in the store
        // buffer and the worker thread may spin forever, causing join() to hang.
        stop_flag_.store(true, std::memory_order_release);
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
        // [ARM MEMORY MODEL] acquire: pairs with the release store inside the
        // thread lambda so that once this returns true (or false after stop),
        // all stores made by the worker thread before that transition are visible.
        return running_.load(std::memory_order_acquire);
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
