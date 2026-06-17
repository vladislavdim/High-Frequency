#include "Sentinel_Analysis_Core.cpp" // Включаем наше ядро (из предыдущего ответа)
#include "Sentinel_Network.hpp"
#include "Sentinel_Parser.hpp"

#include <iostream>
#include <memory>
#include <thread>
#include <boost/asio/ssl.hpp>

using namespace SentinelCore;

int main() {
    std::cout << "==========================================\n";
    std::cout << " SENTINEL PLATFORM (LIVE DEPLOYMENT)\n";
    std::cout << " STATUS: INITIATING SECURE SOCKETS\n";
    std::cout << "==========================================\n";

    // 1. Инициализация Математического Движка
    // Буфер на 100,000 тиков, порог кита = 10 BTC (для теста), порог Z-Score = 3.5
    std::shared_ptr<MarketAnalysisEngine> engine = 
        std::make_shared<MarketAnalysisEngine>(100000, 10.0, 3.5);

    // 2. Инициализация сетевого контекста и SSL (Защита соединения)
    net::io_context ioc;
    ssl::context ctx{ssl::context::tlsv12_client};
    ctx.set_default_verify_paths();
    ctx.set_verify_mode(ssl::verify_peer);

    // 3. Callback: Что делать, когда биржа присылает данные?
    auto on_market_tick = [&engine](const std::string& raw_json) {
        MarketTick tick;
        
        // Передаем JSON в парсер
        if (MarketDataParser::parseBinanceStream(raw_json, tick)) {
            
            // Если парсинг успешен, отдаем тик в математическое ядро
            engine->processTick(tick);
            
            // Запрашиваем анализ ситуации
            AnalysisReport report = engine->generateLiveReport();

            // Логика перехвата движения цены
            if (report.risk_level == "CRITICAL_MANIPULATION") {
                std::cout << "\n[CRITICAL ALERT] MARKET MAKER INTERVENTION DETECTED!\n";
                std::cout << "-> Time: " << report.timestamp << "\n";
                std::cout << "-> VWAP Target: " << report.vwap << "\n";
                std::cout << "-> Order Flow: " << (report.order_flow_imbalance > 0 ? "BULLISH (UP)" : "BEARISH (DOWN)") << "\n";
                std::cout << "-> Z-Score Anomaly: " << report.z_score << " sigma\n";
                std::cout << "------------------------------------------\n";
            }
        }
    };

    // 4. Подключение к Binance (Live Stream: BTC/USDT Trade)
    std::make_shared<WebSocketClient>(ioc, ctx, on_market_tick)->run(
        "stream.binance.com", 
        "9443", 
        "/stream?streams=btcusdt@trade"
    );

    // 5. Запуск многопоточного обработчика (Event Loop)
    std::cout << "[SYSTEM] Handing execution to Boost.Asio I/O Context...\n";
    std::vector<std::thread> v;
    auto const threads = std::thread::hardware_concurrency();
    
    for(auto i = threads - 1; i > 0; --i)
        v.emplace_back([&ioc] { ioc.run(); });
    
    ioc.run();

    return EXIT_SUCCESS;
}
