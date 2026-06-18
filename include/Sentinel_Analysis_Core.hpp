#pragma once
// Sentinel_Analysis_Core.hpp
//
// The mathematical core. Every class here is a pure, deterministic
// calculator: feed it ticks, read back numbers. No I/O, no networking,
// so it is trivially unit-testable and safe to call from the hot path.
//
// IMPORTANT - read before wiring this into anything that risks real money:
// `SmartMoneySignal::confidence` and `direction` are a HEURISTIC composite
// score built from OFI + volume Z-score + liquidity-vacuum bias. It is a
// statistical *tell*, not a forecast. Nothing in this file predicts price
// with certainty, and it should not be presented to an end user as one.

#include <shared_mutex>
#include <deque>
#include <array>
#include <cstdint>
#include <chrono>

#include "Sentinel_Types.hpp"
#include "Sentinel_LockFreeRingBuffer.hpp"

namespace sentinel {

// ---------------------------------------------------------------------------
// RollingStats: O(1) push/evict mean & variance over a fixed-size window.
// Used to compute the Z-score of trade volume ("is this trade abnormally
// large versus recent history?").
// ---------------------------------------------------------------------------
class RollingStats {
public:
    explicit RollingStats(std::size_t window_size);

    // Pushes a new sample, evicting the oldest if the window is full.
    // Returns the Z-score of `value` against the window's state
    // *before* this sample was inserted (so the very first abnormal
    // print already gets flagged, not absorbed into its own baseline).
    double push_and_score(double value);

    double mean() const noexcept;
    double stddev() const noexcept;
    std::size_t count() const noexcept { return window_.size(); }

private:
    std::size_t capacity_;
    std::deque<double> window_;
    double sum_{0.0};
    double sum_sq_{0.0};
};

// ---------------------------------------------------------------------------
// VWAPCalculator: maintains both a session-cumulative VWAP and a short
// rolling-window VWAP (the latter behaves like a fast "fair value" anchor
// that mean-reversion / target levels are measured against).
// ---------------------------------------------------------------------------
class VWAPCalculator {
public:
    explicit VWAPCalculator(std::size_t rolling_window_trades);

    void on_trade(double price, double quantity);

    double session_vwap() const noexcept;
    double window_vwap() const noexcept;

    void reset_session() noexcept;

private:
    // Session (cumulative since reset_session / process start)
    double session_pv_{0.0};
    double session_v_{0.0};

    // Rolling window over the last N trades
    struct PV { double price_volume; double volume; };
    std::size_t window_capacity_;
    std::deque<PV> window_;
    double window_pv_sum_{0.0};
    double window_v_sum_{0.0};
};

// ---------------------------------------------------------------------------
// OFICalculator: best-of-book Order Flow Imbalance, Cont/Kukanov/Stoikov
// (2014) formulation:
//
//   dW_bid =  bid_size_n * 1[P_bid_n >= P_bid_{n-1}]
//           - bid_size_{n-1} * 1[P_bid_n <= P_bid_{n-1}]
//
//   dW_ask =  ask_size_n * 1[P_ask_n <= P_ask_{n-1}]
//           - ask_size_{n-1} * 1[P_ask_n >= P_ask_{n-1}]
//
//   OFI_n = dW_bid - dW_ask
//
// Positive OFI => buy-side pressure building (bids replenishing/improving
// faster than asks); negative => sell-side pressure. We also keep an
// exponentially-decayed running sum so a burst of imbalance doesn't vanish
// the instant a single depth update normalizes.
// ---------------------------------------------------------------------------
class OFICalculator {
public:
    explicit OFICalculator(double ema_decay = 0.94);

    // Feed top-of-book bid/ask. Returns the instantaneous OFI for this update.
    double on_depth_update(double bid_price, double bid_size,
                            double ask_price, double ask_size);

    double ema() const noexcept { return ema_; }
    double normalized() const noexcept; // squashed to roughly [-1, 1] via tanh

private:
    bool has_prev_{false};
    double prev_bid_price_{0.0}, prev_bid_size_{0.0};
    double prev_ask_price_{0.0}, prev_ask_size_{0.0};

    double ema_decay_;
    double ema_{0.0};
    double scale_{1.0}; // running average size, used to normalize before tanh
};

// ---------------------------------------------------------------------------
// LiquidityVacuumDetector: flags the moment resting size is pulled off the
// top levels of the book faster than its recent baseline - the classic
// signature of a large player clearing the way before an aggressive move,
// or of stop-hunting liquidity being yanked just ahead of a sweep.
// ---------------------------------------------------------------------------
class LiquidityVacuumDetector {
public:
    explicit LiquidityVacuumDetector(std::size_t baseline_window = 200,
                                      double trigger_zscore = 3.0);

    struct Result {
        bool triggered{false};
        Side side{Side::Buy}; // side whose depth vanished
        double magnitude{0.0}; // z-score of the depth drop
    };

    Result on_depth_update(double bid_depth_sum, double ask_depth_sum);

private:
    double trigger_z_;
    RollingStats bid_delta_stats_;
    RollingStats ask_delta_stats_;
    double prev_bid_depth_{-1.0};
    double prev_ask_depth_{-1.0};
};

// ---------------------------------------------------------------------------
// AnalysisEngine: wires the above calculators together, owns the
// shared_mutex-protected published snapshot, and is the single object the
// orchestrator (Sentinel_Main) talks to.
// ---------------------------------------------------------------------------
class AnalysisEngine {
public:
    struct Config {
        std::size_t volume_zscore_window = 200;
        std::size_t vwap_rolling_trades = 500;
        std::size_t vacuum_baseline_window = 200;
        double vacuum_trigger_zscore = 3.0;
        double ofi_ema_decay = 0.94;
        double signal_threshold = 0.35; // |composite score| above which we call a direction
    };

    explicit AnalysisEngine(Config config = {});

    // Called from the analysis thread only (single consumer of the ring buffer).
    void on_tick(const MarketTick& tick);

    // Thread-safe snapshot read - safe to call from the JSON-publishing
    // thread concurrently with on_tick() running on the analysis thread.
    SmartMoneySignal snapshot() const;

private:
    void process_trade(const MarketTick& tick);
    void process_depth(const MarketTick& tick);
    void recompute_signal(int64_t event_ts_ms);

    Config cfg_;

    RollingStats volume_stats_;
    VWAPCalculator vwap_;
    OFICalculator ofi_;
    LiquidityVacuumDetector vacuum_;

    double inflow_{0.0};
    double outflow_{0.0};
    double last_price_{0.0};
    std::uint64_t ticks_processed_{0};

    // Decay inflow/outflow so the gauge reflects *recent* pressure, not the
    // entire session (otherwise the very first hour dominates forever).
    static constexpr double kFlowDecay = 0.999;

    mutable std::shared_mutex mutex_;
    SmartMoneySignal published_; // guarded by mutex_
};

} // namespace sentinel
