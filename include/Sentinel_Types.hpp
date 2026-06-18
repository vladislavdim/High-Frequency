#pragma once
// Sentinel_Types.hpp
// Common value types shared across the network, parser and analysis layers.
// Kept POD-like and copyable so they can move through the lock-free queue
// without touching the heap.

#include <cstdint>
#include <array>
#include <string>

namespace sentinel {

enum class TickKind : uint8_t {
    Trade = 0,
    DepthUpdate = 1
};

enum class Side : uint8_t {
    Buy = 0,   // aggressor bought (hit the ask)
    Sell = 1   // aggressor sold (hit the bid)
};

enum class Direction : uint8_t {
    Bullish = 0,
    Bearish = 1,
    Neutral = 2
};

// Top-of-book snapshot used for Order Flow Imbalance (OFI).
// Binance partial-depth streams give us the best N levels; we only need
// best bid / best ask for the classic Cont-Kukanov-Stoikov OFI formula,
// but we keep a few extra levels for the liquidity-vacuum detector.
constexpr std::size_t kDepthLevels = 5;

struct DepthLevel {
    double price{0.0};
    double size{0.0};
};

// A single normalized market event. Produced by Sentinel_Parser,
// consumed by AnalysisEngine. 64-byte-ish footprint by design so a
// handful fit per cache line in the ring buffer.
struct MarketTick {
    TickKind kind{TickKind::Trade};
    Side side{Side::Buy};
    int64_t exchange_ts_ms{0};   // exchange-reported event time
    int64_t local_ts_ns{0};      // local steady_clock arrival time (for latency audit)

    // Trade fields
    double price{0.0};
    double quantity{0.0};

    // Depth fields (only populated when kind == DepthUpdate)
    std::array<DepthLevel, kDepthLevels> bids{};
    std::array<DepthLevel, kDepthLevels> asks{};
    uint8_t bid_count{0};
    uint8_t ask_count{0};
};

// Snapshot of the analysis core's current view of the market.
// This is what gets serialized to www/smart_money.json on every publish tick.
struct SmartMoneySignal {
    int64_t timestamp_ms{0};

    double last_price{0.0};
    double vwap_session{0.0};
    double vwap_window{0.0};      // short rolling-window VWAP (acts as "fair value")

    double volume_zscore{0.0};    // z-score of the most recent trade volume vs rolling window
    double ofi_raw{0.0};          // raw order flow imbalance (top-of-book)
    double ofi_normalized{0.0};   // OFI scaled to roughly [-1, 1]

    double inflow{0.0};           // rolling buy-side notional volume
    double outflow{0.0};          // rolling sell-side notional volume

    bool vacuum_detected{false};
    Side vacuum_side{Side::Buy};  // which side of the book got pulled
    double vacuum_magnitude{0.0};

    Direction direction{Direction::Neutral};
    double confidence{0.0};       // 0-100 heuristic score, NOT a statistical probability

    double target_up{0.0};        // projected upside reference level
    double target_down{0.0};      // projected downside reference level

    uint64_t ticks_processed{0};
    double latency_p99_us{0.0};   // observed end-to-end processing latency
};

} // namespace sentinel
