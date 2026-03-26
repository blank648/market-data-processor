// Copyright (c) 2026 Market Data Processor Project. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "MarketTick.hpp"

namespace mdp {

// ═════════════════════════════════════════════════════════════════════════════
// Concepts
// ═════════════════════════════════════════════════════════════════════════════

/// @brief Constrains T to tick-like types that expose `price` and `timestamp_ns`.
///
/// Any type satisfying this concept must have:
///   - A `double` member named `price`.
///   - A member named `timestamp_ns` convertible to `int64_t`.
template <typename T>
concept TickLike = requires(T t) {
    { t.price } -> std::convertible_to<double>;
    { t.timestamp_ns } -> std::convertible_to<int64_t>;
};

/// @brief Constrains a non-type template parameter to be a power of two.
///
/// Enables bitmask indexing (`index & (N - 1)`) instead of modulo division,
/// which is critical for the hot-path performance of the ring buffer.
template <std::size_t N>
concept PowerOfTwo = (N > 0) && ((N & (N - 1)) == 0);

// ═════════════════════════════════════════════════════════════════════════════
// RingBuffer
// ═════════════════════════════════════════════════════════════════════════════

/// @brief Lock-free, single-producer / single-consumer (SPSC) ring buffer.
///
/// Provides a bounded, wait-free queue for passing tick data between exactly
/// one producer thread and one consumer thread without locks or CAS loops.
/// All operations are O(1) with deterministic latency.
///
/// @tparam T Element type; must satisfy `TickLike`.
/// @tparam N Capacity; must satisfy `PowerOfTwo` (enables bitmask indexing).
///
/// @par Thread Safety
/// - `try_push()` must be called from the **producer thread only**.
/// - `try_pop()` must be called from the **consumer thread only**.
/// - `empty()`, `full()`, `size()`, `capacity()` are safe from any thread
///   but return approximate snapshots — values may be stale by the time
///   the caller acts on them.
template <typename T, std::size_t N>
    requires TickLike<T> && PowerOfTwo<N>
class RingBuffer final {
    // === DESIGN NOTES ====================================================
    //
    // 1. WRAPAROUND ARITHMETIC
    //    head_ and tail_ are monotonically increasing std::size_t counters.
    //    Because unsigned subtraction wraps modulo 2^64, (head - tail)
    //    always yields the correct element count — even if both counters
    //    have wrapped past std::size_t max — as long as the queue never
    //    holds more than 2^64 elements (astronomically impossible with
    //    a capacity bounded by N ≪ 2^64).
    //    Slot indexing uses `counter & (N - 1)` (bitmask, no division).
    //
    // 2. MEMORY ORDERING — NO seq_cst ON THE HOT PATH
    //    On the hot path (try_push / try_pop):
    //    • Own index:  loaded with `relaxed`  — only this thread writes it.
    //    • Other index: loaded with `acquire` — synchronises-with the
    //      `release` store performed by the opposing thread.
    //    • Own index:  stored with `release`  — publishes the write to the
    //      buffer slot so the opposing thread sees a consistent element.
    //    This is the minimal, provably-correct ordering under the C++20
    //    memory model and avoids the full fence implied by seq_cst.
    //    Monitoring helpers (empty/full/size) use seq_cst because they are
    //    off the critical path and need a globally consistent snapshot.
    //
    // 3. CACHE LAYOUT — buffer_ BEFORE head_/tail_
    //    buffer_ is declared first so that hot data lives in the lowest
    //    addresses of the object.  head_ and tail_ follow on their own
    //    cache lines (64-byte aligned) to eliminate false sharing between
    //    producer and consumer.
    //
    // 4. THREAD SAFETY CONTRACT
    //    This is an SPSC queue — exactly ONE producer calls try_push(),
    //    exactly ONE consumer calls try_pop().  Violating this invariant
    //    is undefined behaviour.
    // =====================================================================

public:
    // ─── Construction ─────────────────────────────────────────────────────

    /// @brief Default-constructs the buffer with zero-initialised atomics.
    RingBuffer() = default;

    RingBuffer(const RingBuffer&)            = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;
    RingBuffer(RingBuffer&&)                 = delete;
    RingBuffer& operator=(RingBuffer&&)      = delete;

    // ─── Producer API ─────────────────────────────────────────────────────

    /// @brief Attempts to enqueue an element without blocking.
    ///
    /// @note **PRODUCER THREAD ONLY.**
    ///
    /// @param item Element to move into the buffer.
    /// @return `true` if enqueued; `false` if the buffer is full.
    bool try_push(T&& item) noexcept {
        // [MEMORY MODEL] relaxed: only the producer writes head_, so the
        // local cache always holds the latest value.
        const std::size_t head = head_.value.load(std::memory_order_relaxed);

        // [MEMORY MODEL] acquire: synchronises-with the consumer's release
        // store to tail_, ensuring we see the consumer's most recent pop
        // (and therefore the freed slot).
        const std::size_t tail = tail_.value.load(std::memory_order_acquire);

        if ((head - tail) == N) {
            return false;  // Buffer is full.
        }

        buffer_[head & (N - 1)] = std::move(item);

        // [MEMORY MODEL] release: ensures the buffer write above is visible
        // to the consumer before the consumer sees the incremented head.
        head_.value.store(head + 1, std::memory_order_release);

        return true;
    }

    // ─── Consumer API ─────────────────────────────────────────────────────

    /// @brief Attempts to dequeue an element without blocking.
    ///
    /// @note **CONSUMER THREAD ONLY.**
    ///
    /// @param[out] out Receives the dequeued element via move.
    /// @return `true` if dequeued; `false` if the buffer is empty.
    bool try_pop(T& out) noexcept {
        // [MEMORY MODEL] relaxed: only the consumer writes tail_, so the
        // local cache always holds the latest value.
        const std::size_t tail = tail_.value.load(std::memory_order_relaxed);

        // [MEMORY MODEL] acquire: synchronises-with the producer's release
        // store to head_, ensuring we see the element the producer wrote
        // into the slot before incrementing head.
        const std::size_t head = head_.value.load(std::memory_order_acquire);

        if (head == tail) {
            return false;  // Buffer is empty.
        }

        out = std::move(buffer_[tail & (N - 1)]);

        // [MEMORY MODEL] release: ensures the move-out above completes
        // before the producer sees the incremented tail and reuses the slot.
        tail_.value.store(tail + 1, std::memory_order_release);

        return true;
    }

    // ─── Monitoring (any thread) ──────────────────────────────────────────

    /// @brief Returns `true` if the buffer appears empty (approximate).
    ///
    /// Uses sequentially-consistent loads to obtain a globally consistent
    /// snapshot.  This is off the critical path and intended for monitoring.
    [[nodiscard]] bool empty() const noexcept {
        const std::size_t head = head_.value.load(std::memory_order_seq_cst);
        const std::size_t tail = tail_.value.load(std::memory_order_seq_cst);
        return head == tail;
    }

    /// @brief Returns `true` if the buffer appears full (approximate).
    [[nodiscard]] bool full() const noexcept {
        const std::size_t head = head_.value.load(std::memory_order_seq_cst);
        const std::size_t tail = tail_.value.load(std::memory_order_seq_cst);
        return (head - tail) == N;
    }

    /// @brief Returns the approximate number of elements in the buffer.
    [[nodiscard]] std::size_t size() const noexcept {
        const std::size_t head = head_.value.load(std::memory_order_seq_cst);
        const std::size_t tail = tail_.value.load(std::memory_order_seq_cst);
        return head - tail;
    }

    /// @brief Returns the fixed capacity `N`.
    [[nodiscard]] static constexpr std::size_t capacity() noexcept {
        return N;
    }

private:
    // ─── Storage ──────────────────────────────────────────────────────────
    // buffer_ is placed first for cache-friendliness (frequently accessed
    // elements sit at the lowest address offsets of the object).

    std::array<T, N> buffer_{};

    // ─── Cache-line-isolated indices ──────────────────────────────────────

    /// @brief A std::size_t atomic padded to a full 64-byte cache line.
    ///
    /// Prevents false sharing between the producer's `head_` and the
    /// consumer's `tail_`, which reside on separate cache lines.
    struct alignas(64) AlignedIndex {
        std::atomic<std::size_t> value{0};
        std::byte _pad[64 - sizeof(std::atomic<std::size_t>)];  // NOLINT
    };

    AlignedIndex head_;  ///< Write index — modified by the producer only.
    AlignedIndex tail_;  ///< Read index  — modified by the consumer only.
};

// ═════════════════════════════════════════════════════════════════════════════
// Convenience aliases
// ═════════════════════════════════════════════════════════════════════════════

/// @brief A RingBuffer specialised for `MarketTick` with configurable capacity.
template <std::size_t N>
using TickRingBuffer = RingBuffer<MarketTick, N>;

/// @brief 4 096-slot tick ring buffer (≈160 KiB of tick data).
using TickRingBuffer4K = TickRingBuffer<4096>;

/// @brief 16 384-slot tick ring buffer (≈640 KiB of tick data).
using TickRingBuffer16K = TickRingBuffer<16384>;

}  // namespace mdp
