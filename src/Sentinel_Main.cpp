// Sentinel_Main.cpp
//
// Orchestrator. Wiring, not math:
//   [IO threads: Boost.Asio]  -- WS frames -->  parser
//        -> LockFreeRingBuffer<MarketTick>  -->  [analysis thread]
//                                                     -> AnalysisEngine
//   [publisher thread] -- every PUBLISH_INTERVAL --> engine.snapshot()
//                                                  -> www/smart_money.json
//
// Three independent threads of control, zero shared mutable state except
// the ring buffer (lock-free, SPSC) and AnalysisEngine's published_ snapshot
// (shared_mutex, read-mostly). This is what keeps the network thread's
// latency independent of however long JSON serialization takes.

#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/steady_timer.hpp>

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "Sentinel_Analysis_Core.hpp"
#include "Sentinel_CloudPublisher.hpp"
#include "Sentinel_LockFreeRingBuffer.hpp"
#include "Sentinel_Network.hpp"
#include "Sentinel_Parser.hpp"
#include "Sentinel_Types.hpp"

namespace {

using namespace sentinel;
namespace net = boost::asio;

constexpr std::size_t kRingBufferCapacity = 1 << 14; // 16384, power of two
constexpr auto kPublishInterval = std::chrono::milliseconds(500);

std::optional<std::string> env(const char* name) {
    const char* v = std::getenv(name);
    if (!v || std::string_view(v).empty()) return std::nullopt;
    return std::string(v);
}

std::string direction_to_string(Direction d) {
    switch (d) {
        case Direction::Bullish: return "BULLISH";
        case Direction::Bearish: return "BEARISH";
        default: return "NEUTRAL";
    }
}

std::string side_to_string(Side s) {
    return s == Side::Buy ? "BUY" : "SELL";
}

nlohmann::json signal_to_json(const SmartMoneySignal& s) {
    return {
        {"timestamp_ms", s.timestamp_ms},
        {"last_price", s.last_price},
        {"vwap_session", s.vwap_session},
        {"vwap_window", s.vwap_window},
        {"volume_zscore", s.volume_zscore},
        {"ofi_raw", s.ofi_raw},
        {"ofi_normalized", s.ofi_normalized},
        {"inflow", s.inflow},
        {"outflow", s.outflow},
        {"vacuum", {
            {"detected", s.vacuum_detected},
            {"side", side_to_string(s.vacuum_side)},
            {"magnitude_zscore", s.vacuum_magnitude}
        }},
        {"direction", direction_to_string(s.direction)},
        {"confidence_heuristic", s.confidence},
        {"targets", {
            {"up", s.target_up},
            {"down", s.target_down}
        }},
        {"diagnostics", {
            {"ticks_processed", s.ticks_processed},
            {"latency_p99_us", s.latency_p99_us}
        }},
        {"disclaimer",
            "confidence_heuristic is a statistical signal-strength score "
            "derived from order flow imbalance, volume z-score and liquidity "
            "vacuum detection. It is not a calibrated probability and is not "
            "financial advice."}
    };
}

// Atomic-ish publish: write to a temp file in the same directory, then
// rename over the target. A dashboard polling smart_money.json will never
// observe a half-written file.
void publish_json(const std::filesystem::path& target, const nlohmann::json& doc) {
    const auto tmp_path = target.string() + ".tmp";
    {
        std::ofstream out(tmp_path, std::ios::trunc);
        if (!out) {
            std::cerr << "[Sentinel_Main] failed to open " << tmp_path << " for write\n";
            return;
        }
        out << doc.dump();
    }
    std::error_code ec;
    std::filesystem::rename(tmp_path, target, ec);
    if (ec) {
        std::cerr << "[Sentinel_Main] rename failed: " << ec.message() << "\n";
    }
}

} // namespace

int main(int argc, char** argv) {
    std::string symbol = "btcusdt";
    std::string output_path = "www/smart_money.json";
    if (argc > 1) symbol = argv[1];
    if (argc > 2) output_path = argv[2];

    std::cout << "Sentinel Analysis Core starting | symbol=" << symbol
              << " | output=" << output_path << "\n";

    // ---- Shared state between threads -------------------------------------
    LockFreeRingBuffer<MarketTick, kRingBufferCapacity> tick_queue;
    AnalysisEngine engine{};
    std::atomic<bool> running{true};
    std::atomic<ConnectionState> conn_state{ConnectionState::Connecting};

    // ---- Network layer (IO threads) ----------------------------------------
    net::io_context ioc;

    WebSocketClient::Config ws_cfg;
    ws_cfg.host = "stream.binance.com";
    ws_cfg.port = "9443";
    ws_cfg.target = "/stream?streams=" + symbol + "@trade/" + symbol + "@depth20@100ms";

    auto client = WebSocketClient::create(
        ioc, ws_cfg,
        /*on_frame=*/[&tick_queue](std::string_view frame) {
            // Hot path: parse, then hand off. Never blocks - if the ring
            // buffer is momentarily full we drop the tick rather than stall
            // the network thread (dropped_count() is exposed for monitoring).
            if (auto tick = parser::parse_frame(frame)) {
                tick_queue.try_push(*tick);
            }
        },
        /*on_state=*/[&conn_state](ConnectionState state) {
            conn_state.store(state, std::memory_order_relaxed);
        });

    client->run();

    std::vector<std::thread> io_threads;
    const unsigned io_thread_count = std::max(2u, std::thread::hardware_concurrency() / 4);
    for (unsigned i = 0; i < io_thread_count; ++i) {
        io_threads.emplace_back([&ioc] { ioc.run(); });
    }

    // ---- Analysis thread (single consumer of the ring buffer) -------------
    std::thread analysis_thread([&] {
        int idle_spins = 0;
        while (running.load(std::memory_order_relaxed)) {
            if (auto tick = tick_queue.try_pop()) {
                engine.on_tick(*tick);
                idle_spins = 0;
            } else {
                // Brief adaptive backoff: spin a little (catches bursts
                // without a syscall), then yield to the scheduler so an idle
                // feed doesn't pin a core at 100%.
                if (++idle_spins < 200) {
                    std::this_thread::yield();
                } else {
                    std::this_thread::sleep_for(std::chrono::microseconds(200));
                }
            }
        }
    });

    // ---- Publisher sink: cloud (Firebase RTDB) if configured, else local file ----
    // Reads config from environment variables so the write secret never has
    // to live in source code or get committed to a (possibly public) repo:
    //   FIREBASE_DB_HOST   e.g. "your-project-default-rtdb.firebaseio.com"
    //   FIREBASE_AUTH_SECRET   your Firebase legacy Database Secret
    //   FIREBASE_DB_PATH   optional, defaults to "/smart_money.json"
    std::optional<CloudPublisher> cloud;
    if (auto host = env("FIREBASE_DB_HOST")) {
        CloudPublisher::Config cloud_cfg;
        cloud_cfg.host = *host;
        cloud_cfg.path = env("FIREBASE_DB_PATH").value_or("/smart_money.json");
        cloud_cfg.auth_secret = env("FIREBASE_AUTH_SECRET").value_or("");
        if (cloud_cfg.auth_secret.empty()) {
            std::cerr << "[Sentinel_Main] FIREBASE_DB_HOST is set but FIREBASE_AUTH_SECRET "
                         "is missing - refusing to push unauthenticated. Falling back to "
                         "local file output.\n";
        } else {
            cloud.emplace(std::move(cloud_cfg));
            std::cout << "[Sentinel_Main] publishing to Firebase RTDB at " << *host << "\n";
        }
    }

    const std::filesystem::path out_path(output_path);
    if (!cloud) {
        std::filesystem::create_directories(out_path.parent_path().empty()
                                                 ? std::filesystem::path(".")
                                                 : out_path.parent_path());
        std::cout << "[Sentinel_Main] FIREBASE_DB_HOST not set - writing locally to "
                  << output_path << " instead.\n";
    }

    std::thread publisher_thread([&] {
        while (running.load(std::memory_order_relaxed)) {
            auto snap = engine.snapshot();
            auto doc = signal_to_json(snap);
            doc["connection_state"] = [&] {
                switch (conn_state.load(std::memory_order_relaxed)) {
                    case ConnectionState::Connected: return "LIVE";
                    case ConnectionState::Connecting: return "CONNECTING";
                    case ConnectionState::Disconnected: return "RECONNECTING";
                    default: return "OFFLINE";
                }
            }();
            doc["ring_buffer_dropped"] = tick_queue.dropped_count();

            if (cloud) {
                if (!cloud->publish(doc.dump())) {
                    std::cerr << "[Sentinel_Main] cloud publish failed this cycle - "
                                 "will retry next interval\n";
                }
            } else {
                publish_json(out_path, doc);
            }

            std::this_thread::sleep_for(kPublishInterval);
        }
    });

    // ---- Signal handling for clean shutdown --------------------------------
    net::signal_set signals(ioc, SIGINT, SIGTERM);
    signals.async_wait([&](const boost::system::error_code&, int) {
        std::cout << "\n[Sentinel_Main] shutting down...\n";
        running.store(false, std::memory_order_relaxed);
        client->stop();
        ioc.stop();
    });

    for (auto& t : io_threads) t.join();
    running.store(false, std::memory_order_relaxed);
    analysis_thread.join();
    publisher_thread.join();

    std::cout << "[Sentinel_Main] stopped. Ticks processed: "
              << engine.snapshot().ticks_processed << "\n";
    return 0;
}
