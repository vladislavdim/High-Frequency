// Sentinel_Analysis_Core.cpp
#include "Sentinel_Analysis_Core.hpp"

#include <cmath>
#include <algorithm>

namespace sentinel {

// ============================================================================
// RollingStats
// ============================================================================
RollingStats::RollingStats(std::size_t window_size)
    : capacity_(window_size) {}

double RollingStats::push_and_score(double value) {
    double z = 0.0;
    if (window_.size() >= 2) {
        const double m = mean();
        const double sd = stddev();
        if (sd > 1e-9) {
            z = (value - m) / sd;
        }
    }

    window_.push_back(value);
    sum_ += value;
    sum_sq_ += value * value;

    if (window_.size() > capacity_) {
        const double oldest = window_.front();
        window_.pop_front();
        sum_ -= oldest;
        sum_sq_ -= oldest * oldest;
    }

    return z;
}

double RollingStats::mean() const noexcept {
    if (window_.empty()) return 0.0;
    return sum_ / static_cast<double>(window_.size());
}

double RollingStats::stddev() const noexcept {
    const std::size_t n = window_.size();
    if (n < 2) return 0.0;
    const double m = sum_ / static_cast<double>(n);
    // E[x^2] - (E[x])^2, clamped at 0 to guard against float underflow
    double variance = (sum_sq_ / static_cast<double>(n)) - (m * m);
    variance = std::max(variance, 0.0);
    return std::sqrt(variance);
}

// ============================================================================
// VWAPCalculator
// ============================================================================
VWAPCalculator::VWAPCalculator(std::size_t rolling_window_trades)
    : window_capacity_(rolling_window_trades) {}

void VWAPCalculator::on_trade(double price, double quantity) {
    const double pv = price * quantity;

    session_pv_ += pv;
    session_v_ += quantity;

    window_.push_back(PV{pv, quantity});
    window_pv_sum_ += pv;
    window_v_sum_ += quantity;

    if (window_.size() > window_capacity_) {
        const PV oldest = window_.front();
        window_.pop_front();
        window_pv_sum_ -= oldest.price_volume;
        window_v_sum_ -= oldest.volume;
    }
}

double VWAPCalculator::session_vwap() const noexcept {
    return session_v_ > 1e-12 ? session_pv_ / session_v_ : 0.0;
}

double VWAPCalculator::window_vwap() const noexcept {
    return window_v_sum_ > 1e-12 ? window_pv_sum_ / window_v_sum_ : 0.0;
}

void VWAPCalculator::reset_session() noexcept {
    session_pv_ = 0.0;
    session_v_ = 0.0;
}

// ============================================================================
// OFICalculator
// ============================================================================
OFICalculator::OFICalculator(double ema_decay) : ema_decay_(ema_decay) {}

double OFICalculator::on_depth_update(double bid_price, double bid_size,
                                       double ask_price, double ask_size) {
    double ofi = 0.0;

    if (has_prev_) {
        double dW_bid = 0.0;
        if (bid_price > prev_bid_price_) {
            dW_bid = bid_size;
        } else if (bid_price == prev_bid_price_) {
            dW_bid = bid_size - prev_bid_size_;
        } else { // bid_price < prev_bid_price_
            dW_bid = -prev_bid_size_;
        }

        double dW_ask = 0.0;
        if (ask_price < prev_ask_price_) {
            dW_ask = ask_size;
        } else if (ask_price == prev_ask_price_) {
            dW_ask = ask_size - prev_ask_size_;
        } else { // ask_price > prev_ask_price_
            dW_ask = -prev_ask_size_;
        }

        ofi = dW_bid - dW_ask;

        // Track a running average size scale so normalized() isn't tied to
        // one asset's arbitrary lot size.
        const double observed_scale = (bid_size + ask_size) / 2.0;
        scale_ = scale_ * 0.98 + observed_scale * 0.02;
        ema_ = ema_ * ema_decay_ + ofi * (1.0 - ema_decay_);
    }

    prev_bid_price_ = bid_price; prev_bid_size_ = bid_size;
    prev_ask_price_ = ask_price; prev_ask_size_ = ask_size;
    has_prev_ = true;

    return ofi;
}

double OFICalculator::normalized() const noexcept {
    const double denom = std::max(scale_, 1e-6);
    return std::tanh(ema_ / (denom * 4.0)); // squashed to roughly [-1, 1]
}

// ============================================================================
// LiquidityVacuumDetector
// ============================================================================
LiquidityVacuumDetector::LiquidityVacuumDetector(std::size_t baseline_window,
                                                   double trigger_zscore)
    : trigger_z_(trigger_zscore),
      bid_delta_stats_(baseline_window),
      ask_delta_stats_(baseline_window) {}

LiquidityVacuumDetector::Result
LiquidityVacuumDetector::on_depth_update(double bid_depth_sum, double ask_depth_sum) {
    Result result{};

    if (prev_bid_depth_ >= 0.0) {
        // Negative delta = depth was pulled. We score the *magnitude of
        // withdrawal* (positive number) so a high z-score always means
        // "liquidity vanished fast", regardless of sign convention.
        const double bid_withdrawal = prev_bid_depth_ - bid_depth_sum;
        const double ask_withdrawal = prev_ask_depth_ - ask_depth_sum;

        const double bid_z = bid_delta_stats_.push_and_score(bid_withdrawal);
        const double ask_z = ask_delta_stats_.push_and_score(ask_withdrawal);

        if (bid_z > trigger_z_ && bid_z >= ask_z) {
            result.triggered = true;
            result.side = Side::Sell; // bids pulled -> downside vacuum
            result.magnitude = bid_z;
        } else if (ask_z > trigger_z_) {
            result.triggered = true;
            result.side = Side::Buy; // asks pulled -> upside vacuum
            result.magnitude = ask_z;
        }
    }

    prev_bid_depth_ = bid_depth_sum;
    prev_ask_depth_ = ask_depth_sum;
    return result;
}

// ============================================================================
// AnalysisEngine
// ============================================================================
AnalysisEngine::AnalysisEngine(Config config)
    : cfg_(config),
      volume_stats_(config.volume_zscore_window),
      vwap_(config.vwap_rolling_trades),
      ofi_(config.ofi_ema_decay),
      vacuum_(config.vacuum_baseline_window, config.vacuum_trigger_zscore) {}

void AnalysisEngine::on_tick(const MarketTick& tick) {
    const auto t0 = std::chrono::steady_clock::now();

    if (tick.kind == TickKind::Trade) {
        process_trade(tick);
    } else {
        process_depth(tick);
    }

    ++ticks_processed_;
    recompute_signal(tick.exchange_ts_ms);

    const auto t1 = std::chrono::steady_clock::now();
    const double latency_us =
        std::chrono::duration<double, std::micro>(t1 - t0).count();

    // Cheap streaming max-ish estimate for a p99 readout without keeping a
    // full histogram on the hot path: exponential decay toward the max.
    std::unique_lock lock(mutex_);
    published_.latency_p99_us = std::max(published_.latency_p99_us * 0.995, latency_us);
}

void AnalysisEngine::process_trade(const MarketTick& tick) {
    last_price_ = tick.price;
    vwap_.on_trade(tick.price, tick.quantity);

    const double notional = tick.price * tick.quantity;
    const double z = volume_stats_.push_and_score(tick.quantity);

    inflow_ *= kFlowDecay;
    outflow_ *= kFlowDecay;
    if (tick.side == Side::Buy) {
        inflow_ += notional;
    } else {
        outflow_ += notional;
    }

    // Stash the latest volume z-score directly; recompute_signal reads it
    // back out via the published_ struct rather than a separate member to
    // keep this function self-contained.
    std::unique_lock lock(mutex_);
    published_.volume_zscore = z;
}

void AnalysisEngine::process_depth(const MarketTick& tick) {
    if (tick.bid_count == 0 || tick.ask_count == 0) return;

    const double best_bid_price = tick.bids[0].price;
    const double best_bid_size = tick.bids[0].size;
    const double best_ask_price = tick.asks[0].price;
    const double best_ask_size = tick.asks[0].size;

    ofi_.on_depth_update(best_bid_price, best_bid_size, best_ask_price, best_ask_size);

    double bid_depth_sum = 0.0, ask_depth_sum = 0.0;
    for (uint8_t i = 0; i < tick.bid_count; ++i) bid_depth_sum += tick.bids[i].size;
    for (uint8_t i = 0; i < tick.ask_count; ++i) ask_depth_sum += tick.asks[i].size;

    const auto vacuum_result = vacuum_.on_depth_update(bid_depth_sum, ask_depth_sum);

    std::unique_lock lock(mutex_);
    published_.vacuum_detected = vacuum_result.triggered;
    published_.vacuum_side = vacuum_result.side;
    published_.vacuum_magnitude = vacuum_result.magnitude;
}

void AnalysisEngine::recompute_signal(int64_t event_ts_ms) {
    std::unique_lock lock(mutex_);

    published_.timestamp_ms = event_ts_ms;
    published_.last_price = last_price_;
    published_.vwap_session = vwap_.session_vwap();
    published_.vwap_window = vwap_.window_vwap();
    published_.ofi_raw = ofi_.ema();
    published_.ofi_normalized = ofi_.normalized();
    published_.inflow = inflow_;
    published_.outflow = outflow_;
    published_.ticks_processed = ticks_processed_;

    // Composite heuristic score in roughly [-1, 1]:
    //  - OFI carries the most weight (directly observes order-book pressure)
    //  - volume Z-score adds confidence when the move is backed by abnormal size
    //  - a liquidity vacuum nudges the score toward the side the vacuum implies
    const double z_component =
        std::tanh(published_.volume_zscore / 4.0) *
        (published_.ofi_normalized >= 0 ? 1.0 : -1.0) * 0.3;

    double vacuum_component = 0.0;
    if (published_.vacuum_detected) {
        // Buy-side vacuum (asks pulled) is bullish; sell-side vacuum is bearish.
        vacuum_component = (published_.vacuum_side == Side::Buy ? 1.0 : -1.0) *
                            std::min(published_.vacuum_magnitude / 6.0, 1.0) * 0.25;
    }

    const double score = (published_.ofi_normalized * 0.55) + z_component + vacuum_component;

    if (score > cfg_.signal_threshold) {
        published_.direction = Direction::Bullish;
    } else if (score < -cfg_.signal_threshold) {
        published_.direction = Direction::Bearish;
    } else {
        published_.direction = Direction::Neutral;
    }

    // Confidence is |score| rescaled to 0-100. This is an internal signal
    // *strength* readout, not a calibrated probability of price moving.
    published_.confidence = std::clamp(std::abs(score) * 100.0, 0.0, 100.0);

    // Reference target levels: short-window VWAP +/- a band sized from the
    // recent volume volatility. These are statistical reference levels for
    // where reversion/extension is plausible, not a guaranteed destination.
    const double band = published_.vwap_window *
                         (0.0015 + 0.0010 * std::min(std::abs(published_.ofi_normalized), 1.0));
    published_.target_up = published_.vwap_window + band;
    published_.target_down = published_.vwap_window - band;
}

SmartMoneySignal AnalysisEngine::snapshot() const {
    std::shared_lock lock(mutex_);
    return published_;
}

} // namespace sentinel
