#pragma once

#include <cstdint>
#include <string_view>

namespace mdp {

/// @brief Pure abstract interface for market data feed sources.
///
/// Decouples feed sources from the rest of the pipeline to allow for multiple
/// implementations.
///
/// @par ExtensionNote
/// Both a FeedSimulator and future WebSocket-based adapters (e.g., Binance, Alpaca)
/// will implement this IFeedSource interface.
class IFeedSource {
public:
    /// @brief Virtual destructor for proper polymorphic cleanup.
    virtual ~IFeedSource() = default;

    /// @brief Starts the feed source.
    virtual void start() = 0;

    /// @brief Stops the feed source.
    virtual void stop() = 0;

    /// @brief Checks if the feed source is currently running.
    /// @return true if running, false otherwise.
    [[nodiscard]] virtual bool is_running() const noexcept = 0;

    /// @brief Retrieves the descriptive name of the feed source.
    /// @return A string view representing the source's name.
    [[nodiscard]] virtual std::string_view source_name() const noexcept = 0;

    /// @brief Gets the total number of ticks successfully published.
    /// @return The published tick count. Default implementation returns 0.
    virtual uint64_t ticks_published() const noexcept { 
        return 0; 
    }

    /// @brief Gets the total number of ticks dropped (e.g., due to a full buffer).
    /// @return The dropped tick count. Default implementation returns 0.
    virtual uint64_t ticks_dropped() const noexcept { 
        return 0; 
    }

    // Non-copyable (interface semantics)
    IFeedSource(const IFeedSource&) = delete;
    IFeedSource& operator=(const IFeedSource&) = delete;

    // Non-movable (interface semantics)
    IFeedSource(IFeedSource&&) = delete;
    IFeedSource& operator=(IFeedSource&&) = delete;

protected:
    /// @brief Protected default constructor to ensure only subclasses can instantiate.
    IFeedSource() = default;
};

} // namespace mdp
