#pragma once
// Sentinel_CloudPublisher.hpp
//
// Pushes the engine's JSON snapshot to Firebase Realtime Database over
// plain HTTPS (REST API), so a static site (GitHub Pages) can subscribe to
// it in real time without your machine ever serving files itself.
//
// Why REST instead of a "real" Firebase SDK: Firebase has no official C++
// SDK outside of their mobile/game toolchain (and that one pulls in a huge
// dependency tree). The RTDB REST API is just HTTP PUT with a JSON body to
// a `.json` URL, so we reuse Boost.Beast + OpenSSL - already in this
// project's stack - and skip an entire extra SDK.
//
// This class is intentionally *synchronous/blocking*. It is only ever
// called from the dedicated publisher thread in Sentinel_Main, which has
// no latency budget to protect (unlike AnalysisEngine::on_tick on the
// analysis thread). Blocking here costs nothing the rest of the system
// cares about.

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>

#include <optional>
#include <string>

namespace sentinel {

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
using tcp = boost::asio::ip::tcp;

class CloudPublisher {
public:
    struct Config {
        // Host only, no scheme - e.g. "your-project-default-rtdb.firebaseio.com"
        // (copy from your Firebase Console databaseURL, minus "https://" and
        // the trailing slash).
        std::string host;

        // RTDB REST path, must end in ".json" - e.g. "/smart_money.json"
        std::string path = "/smart_money.json";

        // Firebase legacy Database Secret. Treat this like a root password:
        // it bypasses all Security Rules. Read it from an environment
        // variable at startup - NEVER hardcode it, NEVER commit it.
        std::string auth_secret;
    };

    explicit CloudPublisher(Config config);

    // Sends one PUT (full overwrite of the JSON at `path`). Returns false on
    // any network/HTTP error - the connection is torn down on failure so the
    // next call transparently reconnects rather than retrying a broken
    // socket. Caller decides whether to log and move on (recommended: don't
    // let a single failed cloud push crash the whole process).
    bool publish(const std::string& json_body);

private:
    bool ensure_connected();
    void reset_connection();

    Config cfg_;
    net::io_context ioc_;               // only used for the resolver; no .run() needed
    ssl::context ssl_ctx_{ssl::context::tlsv12_client};
    std::optional<beast::ssl_stream<beast::tcp_stream>> stream_;
};

} // namespace sentinel
