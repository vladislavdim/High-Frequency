#pragma once
// Sentinel_Parser.hpp
//
// Deserializes raw text frames from Binance's combined WebSocket stream
// (wss://stream.binance.com:9443/stream?streams=btcusdt@trade/btcusdt@depth20@100ms)
// into MarketTick structs.
//
// Uses nlohmann::json (header-only). If `nlohmann/json.hpp` isn't available
// via your package manager, vendor the single header from
// https://github.com/nlohmann/json (releases ship a single-include amalgam)
// into include/nlohmann/json.hpp - no other change needed.
//
// Kept header-only and allocation-light: we parse straight into a
// stack-allocated MarketTick and never build intermediate strings beyond
// what nlohmann::json itself needs for the numeric/string fields we touch.

#include <nlohmann/json.hpp>
#include <chrono>
#include <optional>
#include <string_view>

#include "Sentinel_Types.hpp"

namespace sentinel::parser {

inline int64_t now_steady_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Binance trade payload (innermost "data" object for the @trade stream):
// { "e":"trade", "E":1718900000123, "s":"BTCUSDT", "p":"67123.45",
//   "q":"0.014", "m": true, ... }
// "m" = true means the buyer is the market maker, i.e. the aggressor SOLD.
inline std::optional<MarketTick> parse_trade(const nlohmann::json& data) {
    if (!data.contains("p") || !data.contains("q")) return std::nullopt;

    MarketTick tick{};
    tick.kind = TickKind::Trade;
    tick.local_ts_ns = now_steady_ns();
    tick.exchange_ts_ms = data.value("E", static_cast<int64_t>(0));

    tick.price = std::stod(data.at("p").get<std::string>());
    tick.quantity = std::stod(data.at("q").get<std::string>());

    const bool buyer_is_maker = data.value("m", false);
    // If the buyer is the maker, the aggressor (taker) sold into the bid.
    tick.side = buyer_is_maker ? Side::Sell : Side::Buy;

    return tick;
}

// Binance partial depth payload (@depthNN@100ms):
// { "lastUpdateId":..., "bids":[["67120.10","0.42"], ...],
//                       "asks":[["67121.00","0.31"], ...] }
inline std::optional<MarketTick> parse_depth(const nlohmann::json& data) {
    if (!data.contains("bids") || !data.contains("asks")) return std::nullopt;

    MarketTick tick{};
    tick.kind = TickKind::DepthUpdate;
    tick.local_ts_ns = now_steady_ns();
    tick.exchange_ts_ms = data.value("E", static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count()));

    const auto& bids = data.at("bids");
    const auto& asks = data.at("asks");

    tick.bid_count = static_cast<uint8_t>(std::min(bids.size(), kDepthLevels));
    tick.ask_count = static_cast<uint8_t>(std::min(asks.size(), kDepthLevels));

    for (uint8_t i = 0; i < tick.bid_count; ++i) {
        tick.bids[i].price = std::stod(bids[i][0].get<std::string>());
        tick.bids[i].size  = std::stod(bids[i][1].get<std::string>());
    }
    for (uint8_t i = 0; i < tick.ask_count; ++i) {
        tick.asks[i].price = std::stod(asks[i][0].get<std::string>());
        tick.asks[i].size  = std::stod(asks[i][1].get<std::string>());
    }

    return tick;
}

// Entry point: takes one raw WebSocket text frame (the combined-stream
// envelope) and returns a parsed MarketTick, if the frame was one we care
// about. Unknown/partial frames are swallowed (return nullopt) rather than
// throwing - a malformed frame should never be able to take the feed down.
inline std::optional<MarketTick> parse_frame(std::string_view raw) {
    try {
        const auto envelope = nlohmann::json::parse(raw, nullptr, /*allow_exceptions=*/true);

        // Combined stream wraps payloads as {"stream": "...", "data": {...}}.
        // Direct single-stream connections send the payload at the top level.
        const nlohmann::json& data = envelope.contains("data") ? envelope.at("data") : envelope;
        const std::string stream_name = envelope.value("stream", std::string{});

        if (data.contains("e") && data.at("e") == "trade") {
            return parse_trade(data);
        }
        if (stream_name.find("@depth") != std::string::npos || data.contains("bids")) {
            return parse_depth(data);
        }
        return std::nullopt;
    } catch (const nlohmann::json::exception&) {
        return std::nullopt;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

} // namespace sentinel::parser
