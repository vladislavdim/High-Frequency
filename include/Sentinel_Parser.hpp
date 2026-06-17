#ifndef SENTINEL_PARSER_HPP
#define SENTINEL_PARSER_HPP

#include <string>
#include <iostream>
// Требуется библиотека nlohmann/json
#include <nlohmann/json.hpp> 

namespace SentinelCore {

    using json = nlohmann::json;

    class MarketDataParser {
    public:
        // Метод для парсинга потока Binance (@depth или @trade)
        static bool parseBinanceStream(const std::string& raw_payload, MarketTick& out_tick) {
            try {
                auto data = json::parse(raw_payload);

                // Защита от системных сообщений биржи
                if (!data.contains("data") || !data["data"].contains("e")) {
                    return false; 
                }

                auto event_type = data["data"]["e"].get<std::string>();

                // Обрабатываем только агрессивные рыночные сделки (Trade)
                if (event_type == "trade") {
                    out_tick.timestamp_ms = data["data"]["T"].get<uint64_t>();
                    out_tick.price = std::stod(data["data"]["p"].get<std::string>());
                    out_tick.volume = std::stod(data["data"]["q"].get<std::string>());
                    out_tick.is_buyer_maker = data["data"]["m"].get<bool>();
                    
                    // Имитация ликвидности (в реальности берется из отдельного потока OrderBook)
                    out_tick.bid_liquidity = 50000000.0; 
                    out_tick.ask_liquidity = 50000000.0;
                    return true;
                }
                return false;

            } catch (const json::parse_error& e) {
                std::cerr << "[PARSER ERROR] JSON Malformed: " << e.what() << '\n';
                return false;
            } catch (const std::exception& e) {
                std::cerr << "[PARSER ERROR] Data extraction failed: " << e.what() << '\n';
                return false;
            }
        }
    };
}

#endif // SENTINEL_PARSER_HPP
