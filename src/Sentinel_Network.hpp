// Sentinel_Network.cpp
#include "Sentinel_Network.hpp"

#include <boost/asio/strand.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/ssl/error.hpp>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <iostream>

namespace sentinel {

std::shared_ptr<WebSocketClient> WebSocketClient::create(net::io_context& ioc,
                                                           Config config,
                                                           FrameHandler on_frame,
                                                           StateHandler on_state) {
    return std::shared_ptr<WebSocketClient>(
        new WebSocketClient(ioc, std::move(config), std::move(on_frame), std::move(on_state)));
}

WebSocketClient::WebSocketClient(net::io_context& ioc, Config config,
                                  FrameHandler on_frame, StateHandler on_state)
    : ioc_(ioc),
      cfg_(std::move(config)),
      on_frame_(std::move(on_frame)),
      on_state_(std::move(on_state)),
      resolver_(net::make_strand(ioc)),
      ws_(net::make_strand(ioc), ssl_ctx_),
      current_backoff_(cfg_.reconnect_initial_backoff) {
    // Trust the system's default CA bundle for exchange TLS certs.
    ssl_ctx_.set_default_verify_paths();
    ssl_ctx_.set_verify_mode(ssl::verify_peer);
}

void WebSocketClient::set_state(ConnectionState state) {
    if (on_state_) on_state_(state);
}

void WebSocketClient::run() {
    stopping_ = false;
    do_resolve();
}

void WebSocketClient::stop() {
    stopping_ = true;
    beast::error_code ec;
    ws_.next_layer().next_layer().socket().close(ec);
}

void WebSocketClient::do_resolve() {
    set_state(ConnectionState::Connecting);
    resolver_.async_resolve(
        cfg_.host, cfg_.port,
        beast::bind_front_handler(&WebSocketClient::on_resolve, shared_from_this()));
}

void WebSocketClient::on_resolve(beast::error_code ec, tcp::resolver::results_type results) {
    if (ec) {
        std::cerr << "[Sentinel_Network] resolve failed: " << ec.message() << "\n";
        schedule_reconnect();
        return;
    }

    beast::get_lowest_layer(ws_).expires_after(std::chrono::seconds(15));
    beast::get_lowest_layer(ws_).async_connect(
        results,
        beast::bind_front_handler(&WebSocketClient::on_connect, shared_from_this()));
}

void WebSocketClient::on_connect(beast::error_code ec, tcp::resolver::results_type::endpoint_type) {
    if (ec) {
        std::cerr << "[Sentinel_Network] connect failed: " << ec.message() << "\n";
        schedule_reconnect();
        return;
    }

    // SNI - required by most TLS-terminating exchange front ends, and
    // Binance specifically rejects handshakes without it.
    if (!SSL_set_tlsext_host_name(ws_.next_layer().native_handle(), cfg_.host.c_str())) {
        ec = beast::error_code(static_cast<int>(::ERR_get_error()),
                                net::error::get_ssl_category());
        std::cerr << "[Sentinel_Network] SNI set failed: " << ec.message() << "\n";
        schedule_reconnect();
        return;
    }

    beast::get_lowest_layer(ws_).expires_after(std::chrono::seconds(15));
    ws_.next_layer().async_handshake(
        ssl::stream_base::client,
        beast::bind_front_handler(&WebSocketClient::on_ssl_handshake, shared_from_this()));
}

void WebSocketClient::on_ssl_handshake(beast::error_code ec) {
    if (ec) {
        std::cerr << "[Sentinel_Network] TLS handshake failed: " << ec.message() << "\n";
        schedule_reconnect();
        return;
    }

    beast::get_lowest_layer(ws_).expires_never();
    ws_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));
    ws_.set_option(websocket::stream_base::decorator([](websocket::request_type& req) {
        req.set(http::field::user_agent, "Sentinel-Analysis-Core/1.0 (+cpp20)");
    }));

    ws_.async_handshake(cfg_.host, cfg_.target,
                         beast::bind_front_handler(&WebSocketClient::on_handshake, shared_from_this()));
}

void WebSocketClient::on_handshake(beast::error_code ec) {
    if (ec) {
        std::cerr << "[Sentinel_Network] WS handshake failed: " << ec.message() << "\n";
        schedule_reconnect();
        return;
    }

    current_backoff_ = cfg_.reconnect_initial_backoff; // reset backoff on success
    set_state(ConnectionState::Connected);
    do_read();
}

void WebSocketClient::do_read() {
    ws_.async_read(buffer_,
                    beast::bind_front_handler(&WebSocketClient::on_read, shared_from_this()));
}

void WebSocketClient::on_read(beast::error_code ec, std::size_t bytes_transferred) {
    if (ec) {
        if (!stopping_) {
            std::cerr << "[Sentinel_Network] read failed: " << ec.message() << "\n";
            set_state(ConnectionState::Disconnected);
            schedule_reconnect();
        }
        return;
    }

    if (on_frame_) {
        const auto data = buffer_.data();
        std::string_view frame(static_cast<const char*>(data.data()), bytes_transferred);
        on_frame_(frame);
    }
    buffer_.consume(bytes_transferred);

    do_read(); // keep the read loop going
}

void WebSocketClient::schedule_reconnect() {
    if (stopping_) return;
    set_state(ConnectionState::Disconnected);

    auto timer = std::make_shared<net::steady_timer>(ioc_, current_backoff_);
    auto self = shared_from_this();
    timer->async_wait([self, timer](beast::error_code ec) {
        if (ec || self->stopping_) return;
        self->current_backoff_ = std::min(self->current_backoff_ * 2,
                                           self->cfg_.reconnect_max_backoff);

        // Fresh resolver/socket state for the retry: tear down and rebuild
        // the websocket stream over a new TCP+TLS layer.
        self->ws_ = websocket::stream<beast::ssl_stream<beast::tcp_stream>>(
            net::make_strand(self->ioc_), self->ssl_ctx_);
        self->buffer_.clear();
        self->do_resolve();
    });
}

} // namespace sentinel
