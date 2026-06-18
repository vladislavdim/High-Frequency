# Sentinel Analysis Core — BTC Smart Money Flow Monitor

A C++20 real-time analytics core for BTC/USDT order flow: lock-free tick
ingestion, VWAP, volume Z-score, top-of-book Order Flow Imbalance (OFI),
and a liquidity-vacuum detector, published as JSON for a live dashboard.

## Honest scope (read this first)

`direction` and `confidence_heuristic` in the output are a **statistical
signal-strength score**, not a forecast. OFI, volume Z-score and liquidity
withdrawal are real, well-studied microstructure signals — they describe
*what already happened* in the order book with more precision than a raw
chart, but no formula here or anywhere "точно" predicts where BTC goes next.
Treat the dashboard as a flow-monitoring instrument, not a crystal ball, and
don't size real positions off `confidence_heuristic` alone. This is not
financial advice.

## Layout

```
include/Sentinel_Types.hpp              shared structs (MarketTick, SmartMoneySignal)
include/Sentinel_LockFreeRingBuffer.hpp SPSC lock-free queue, cache-line aligned
include/Sentinel_Analysis_Core.hpp      VWAP / Z-score / OFI / vacuum / engine (declarations)
src/Sentinel_Analysis_Core.cpp          ...implementations
include/Sentinel_Parser.hpp             Binance JSON -> MarketTick
include/Sentinel_Network.hpp            WSS client (declarations)
src/Sentinel_Network.cpp                ...implementation (Boost.Beast + OpenSSL)
src/Sentinel_Main.cpp                   orchestrator: network -> ring buffer -> engine -> JSON
www/index.html                          dashboard (Lightweight Charts, polls smart_money.json)
CMakeLists.txt
```

## Dependencies

- A C++20 compiler (GCC 12+/Clang 15+)
- Boost ≥ 1.81 (`system`, `thread`; Asio/Beast are header-only and come with Boost)
- OpenSSL dev headers
- nlohmann/json ≥ 3.10 (header-only)
- CMake ≥ 3.20

```bash
# Debian/Ubuntu
sudo apt-get install libboost-all-dev libssl-dev nlohmann-json3-dev cmake build-essential

# macOS
brew install boost openssl nlohmann-json cmake
```

If `nlohmann_json` isn't found by your package manager, drop the single
header from https://github.com/nlohmann/json (releases ship one amalgamated
`json.hpp`) into `include/nlohmann/json.hpp` — no code changes needed.

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

## Run

```bash
./build/sentinel_core btcusdt www/smart_money.json
```

Then open `www/index.html` in a browser (serve the `www/` folder with any
static file server so `fetch('smart_money.json')` isn't blocked by
`file://` CORS rules, e.g. `python3 -m http.server` from inside `www/`).

## Notes / caveats

- **Compiled environment**: this was authored without network/package
  access to actually compile against Boost.Beast in this sandbox. The code
  follows the standard Beast async WSS client pattern closely, but treat it
  as a first pass — if your compiler flags anything (Beast's API does shift
  slightly across Boost versions), send me the error and I'll patch it.
- **Exchange access**: this targets Binance's public market-data WebSocket
  (`stream.binance.com`), which doesn't require an API key for trades/depth.
  Some jurisdictions restrict access to Binance.com directly — check what's
  permitted where you're deploying, and swap the host/target in
  `Sentinel_Main.cpp` for a compliant venue if needed (the parser/engine
  don't care which exchange the JSON came from as long as the field shapes
  match).
- **Latency target**: the <1ms goal applies to the ring-buffer-to-signal
  hot path (`AnalysisEngine::on_tick`), which the code measures and reports
  as `diagnostics.latency_p99_us`. Network round-trip latency to the
  exchange itself is outside this core's control.
