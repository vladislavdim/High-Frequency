#pragma once
// Sentinel_Network.hpp
//
// Async, auto-reconnecting WSS client built on Boost.Beast + Boost.Asio +
// OpenSSL. Owns nothing about parsing or analysis - it hands raw text
// frames to a callback and gets out of the way. All I/O runs on the
// io_context passed in by the orchestrator (Sentinel_Main), which is what
// lets us share a thread pool across many connections if needed later.

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

#include <functional>
#include <memory>
#include <string>

namespace sentinel {

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
using tcp = boost::asio::ip::tcp;

// Called once per WebSocket text frame received. Invoked on whichever
// io_context thread happened to service the read - if your callback does
// anything non-trivial, hand off to your own queue immediately
// (Sentinel_Main does exactly this via the lock-free ring buffer).
using FrameHandler = std::function<void(std::string_view frame)>;

// Called on connect/disconnect transitions, useful for a dashboard
// "LIVE / RECONNECTING / OFFLINE" indicator.
enum class ConnectionState { Connecting, Connected, Disconnected, Failed };
using StateHandler = std::function<void(ConnectionState)>;

class WebSocketClient : public std::enable_shared_from_this<WebSocketClient> {
public:
    struct Config {
        std::string host;            // e.g. "stream.binance.com"
        std::string port = "9443";
        std::string target;          // e.g. "/stream?streams=btcusdt@trade/btcusdt@depth20@100ms"
        std::chrono::milliseconds reconnect_initial_backoff{500};
        std::chrono::milliseconds reconnect_max_backoff{15000};
        std::chrono::seconds ping_interval{15};
    };

    static std::shared_ptr<WebSocketClient> create(net::io_context& ioc,
                                                     Config config,
                                                     FrameHandler on_frame,
                                                     StateHandler on_state = nullptr);

    // Begins the connect -> handshake -> read loop. Safe to call once;
    // reconnects are handled internally after the first call.
    void run();

    // Graceful shutdown - closes the socket and suppresses further
    // reconnect attempts.
    void stop();

private:
    WebSocketClient(net::io_context& ioc, Config config,
                     FrameHandler on_frame, StateHandler on_state);

    void do_resolve();
    void on_resolve(beast::error_code ec, tcp::resolver::results_type results);
    void on_connect(beast::error_code ec, tcp::resolver::results_type::endpoint_type ep);
    void on_ssl_handshake(beast::error_code ec);
    void on_handshake(beast::error_code ec);
    void do_read();
    void on_read(beast::error_code ec, std::size_t bytes_transferred);
    void schedule_reconnect();
    void set_state(ConnectionState state);

    net::io_context& ioc_;
    Config cfg_;
    FrameHandler on_frame_;
    StateHandler on_state_;

    ssl::context ssl_ctx_{ssl::context::tlsv12_client};
    tcp::resolver resolver_;
    websocket::stream<beast::ssl_stream<beast::tcp_stream>> ws_;
    beast::flat_buffer buffer_;

    std::chrono::milliseconds current_backoff_;
    bool stopping_{false};
};

} // namespace sentinel
