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
include/Sentinel_CloudPublisher.hpp     HTTPS push to Firebase RTDB (declarations)
src/Sentinel_CloudPublisher.cpp         ...implementation
src/Sentinel_Main.cpp                   orchestrator: network -> ring buffer -> engine -> cloud/file
www/index.html                          dashboard (Lightweight Charts + live Firebase subscription)
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

## Run (local file mode — default, no Firebase setup required)

```bash
./build/sentinel_core btcusdt www/smart_money.json
```

This is for testing the core itself in isolation — confirm it's running and
producing sane numbers with `watch -n1 cat www/smart_money.json`. Note that
**the bundled `index.html` no longer reads this file** — per the cloud
migration below, it only speaks to Firebase now. To see live data in the
dashboard you need Cloud mode running (next section), even for local testing.

## Cloud mode (Firebase Realtime Database) — for a static GitHub Pages dashboard

By default the core writes `smart_money.json` to disk, which only works if
something is also serving that disk to the browser. If your dashboard lives
on GitHub Pages (no server-side process possible there), the core instead
pushes snapshots straight into Firebase Realtime Database over HTTPS, and
`www/index.html` subscribes to that in real time — no polling, no file.

### 1. Create the Firebase project (one-time, ~2 minutes)

1. https://console.firebase.google.com → "Add project" → name it anything.
2. In the project, go to **Build → Realtime Database → Create Database**.
   Pick a region, start in **locked mode**.
3. Go to **Realtime Database → Rules** and set:
   ```json
   { "rules": { ".read": true, ".write": false } }
   ```
   `write: false` is fine — your C++ core authenticates with the Database
   Secret below, which bypasses rules entirely. This just blocks *anyone
   else* from writing.
4. Go to **Project settings (gear icon) → Service accounts → Database secrets**
   and generate a secret. **Treat it like a root password** — never commit
   it, never put it in `index.html`.
5. Still in **Project settings → General**, scroll to "Your apps" → add a
   **Web app** → copy the `firebaseConfig` object it gives you (apiKey,
   authDomain, databaseURL, projectId). These ARE safe to commit — they
   don't grant access by themselves, the Rules above do that.

### 2. Wire the config into the dashboard

Open `www/index.html`, find the `firebaseConfig` object near the top of the
`<script type="module">` block, and paste in your real values from step 1.5.
Commit and push — GitHub Pages will serve the updated file.

### 3. Run the core with cloud mode enabled

Set the two environment variables before launching (never put the secret on
the command line in shell history or in a script you'll commit):

```bash
export FIREBASE_DB_HOST="your-project-default-rtdb.firebaseio.com"   # from databaseURL, no scheme, no slash
export FIREBASE_AUTH_SECRET="paste-your-database-secret-here"
./build/sentinel_core btcusdt
```

If `FIREBASE_DB_HOST` isn't set, the core falls back to writing
`www/smart_money.json` locally (useful for offline testing) and prints a
warning saying so.

### 4. What the dashboard shows now

- **CONNECTING...** — the browser hasn't established its Firebase socket yet.
- **WAITING FOR CORE...** — Firebase is connected, but nothing's been pushed yet.
- **NO SIGNAL FROM CORE** — was receiving data, but nothing new in 5+ seconds
  (your `sentinel_core` process probably stopped or lost its exchange link).
- **LIVE** (green, pulsing) — fresh data arriving normally.

The exchange-side connection state (is the core itself still talking to
Binance) is shown separately in the small diagnostics line in the footer,
so it's never confused with the page-to-Firebase connection above it.


## Deploy without a local terminal (Render Background Worker)

If you don't have a PC/VPS terminal handy, Render builds and runs this for
you entirely through its web dashboard — the same flow as the APEX bot,
just pointed at this repo's `Dockerfile` instead of a Python runtime.

1. Push everything in this repo (including the new `Dockerfile`) to GitHub.
2. In Render: **New → Background Worker** → connect this repo.
3. Render should auto-detect the `Dockerfile` and offer **Docker** as the
   environment. If it asks for a build/start command, leave both blank —
   the Dockerfile's `ENTRYPOINT`/`CMD` already define how it runs.
4. Under **Environment**, add:
   - `FIREBASE_DB_HOST` = `your-project-default-rtdb.firebaseio.com`
   - `FIREBASE_AUTH_SECRET` = your Database Secret
5. Deploy. Watch the **Logs** tab in the browser — that's your compiler
   output. If the build fails, the exact error will be there; paste it back
   to me and I'll fix the source.

Once it's live, Render keeps the process running continuously (this is what
"always-on" actually requires — a C++ process needs a real, persistent CPU
somewhere; it can't run "in the cloud" abstractly). It will keep pushing to
Firebase, and `www/index.html` on GitHub Pages will pick the data up the
moment it arrives — no redeploy of the site needed for that part.

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
