#ifndef SENTINEL_NETWORK_HPP
#define SENTINEL_NETWORK_HPP

#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/asio/strand.hpp>
#include <functional>
#include <iostream>
#include <string>
#include <memory>

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
using tcp = boost::asio::ip::tcp;

namespace SentinelCore {

    class WebSocketClient : public std::enable_shared_from_this<WebSocketClient> {
        tcp::resolver resolver_;
        websocket::stream<beast::ssl_stream<beast::tcp_stream>> ws_;
        beast::flat_buffer buffer_;
        std::string host_;
        std::function<void(std::string)> on_message_callback_;

    public:
        explicit WebSocketClient(net::io_context& ioc, ssl::context& ctx, 
                                 std::function<void(std::string)> callback)
            : resolver_(net::make_strand(ioc)), ws_(net::make_strand(ioc), ctx), 
              on_message_callback_(std::move(callback)) {}

        void run(const std::string& host, const std::string& port, const std::string& path) {
            host_ = host;
            resolver_.async_resolve(host, port,
                beast::bind_front_handler(&WebSocketClient::on_resolve, shared_from_this(), path));
        }

    private:
        void on_resolve(std::string path, beast::error_code ec, tcp::resolver::results_type results) {
            if (ec) { std::cerr << "[NETWORK] Resolve Error: " << ec.message() << "\n"; return; }
            beast::get_lowest_layer(ws_).async_connect(results,
                beast::bind_front_handler(&WebSocketClient::on_connect, shared_from_this(), path));
        }

        void on_connect(std::string path, beast::error_code ec, tcp::resolver::results_type::endpoint_type ep) {
            if (ec) { std::cerr << "[NETWORK] Connect Error: " << ec.message() << "\n"; return; }
            ws_.next_layer().async_handshake(ssl::stream_base::client,
                beast::bind_front_handler(&WebSocketClient::on_ssl_handshake, shared_from_this(), path));
        }

        void on_ssl_handshake(std::string path, beast::error_code ec) {
            if (ec) { std::cerr << "[NETWORK] SSL Error: " << ec.message() << "\n"; return; }
            ws_.async_handshake(host_, path,
                beast::bind_front_handler(&WebSocketClient::on_handshake, shared_from_this()));
        }

        void on_handshake(beast::error_code ec) {
            if (ec) { std::cerr << "[NETWORK] WS Handshake Error: " << ec.message() << "\n"; return; }
            std::cout << "[SYSTEM] Secure connection established. Stream active.\n";
            do_read();
        }

        void do_read() {
            ws_.async_read(buffer_,
                beast::bind_front_handler(&WebSocketClient::on_read, shared_from_this()));
        }

        void on_read(beast::error_code ec, std::size_t bytes_transferred) {
            boost::ignore_unused(bytes_transferred);
            if (ec) { std::cerr << "[NETWORK] Read Error: " << ec.message() << "\n"; return; }
            
            // Отправляем сырую строку в callback (наш парсер)
            on_message_callback_(beast::buffers_to_string(buffer_.data()));
            buffer_.consume(buffer_.size());
            
            do_read(); // Ждем следующий тик
        }
    };
}

#endif // SENTINEL_NETWORK_HPP
