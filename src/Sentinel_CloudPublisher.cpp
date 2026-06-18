// Sentinel_CloudPublisher.cpp
#include "Sentinel_CloudPublisher.hpp"

#include <openssl/ssl.h>
#include <iostream>

namespace sentinel {

CloudPublisher::CloudPublisher(Config config) : cfg_(std::move(config)) {
    ssl_ctx_.set_default_verify_paths();
    ssl_ctx_.set_verify_mode(ssl::verify_peer);
}

void CloudPublisher::reset_connection() {
    if (stream_) {
        beast::error_code ec;
        stream_->shutdown(ec); // best-effort; we're tearing this down regardless
    }
    stream_.reset();
}

bool CloudPublisher::ensure_connected() {
    if (stream_) return true;

    try {
        tcp::resolver resolver(ioc_);
        const auto results = resolver.resolve(cfg_.host, "443");

        stream_.emplace(ioc_, ssl_ctx_);

        if (!SSL_set_tlsext_host_name(stream_->native_handle(), cfg_.host.c_str())) {
            std::cerr << "[Sentinel_CloudPublisher] SNI set failed\n";
            stream_.reset();
            return false;
        }

        beast::get_lowest_layer(*stream_).expires_after(std::chrono::seconds(10));
        beast::get_lowest_layer(*stream_).connect(results);

        beast::get_lowest_layer(*stream_).expires_after(std::chrono::seconds(10));
        stream_->handshake(ssl::stream_base::client);

        beast::get_lowest_layer(*stream_).expires_never();
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[Sentinel_CloudPublisher] connect failed: " << e.what() << "\n";
        stream_.reset();
        return false;
    }
}

bool CloudPublisher::publish(const std::string& json_body) {
    if (!ensure_connected()) return false;

    try {
        // No auth token needed - Firebase rules allow open writes.
        // The path must end in ".json" per Firebase REST API spec.
        const std::string target = cfg_.path;

        http::request<http::string_body> req{http::verb::put, target, 11};
        req.set(http::field::host, cfg_.host);
        req.set(http::field::user_agent, "Sentinel-Analysis-Core/1.0 (+cpp20)");
        req.set(http::field::content_type, "application/json");
        req.keep_alive(true);
        req.body() = json_body;
        req.prepare_payload();

        beast::get_lowest_layer(*stream_).expires_after(std::chrono::seconds(8));
        http::write(*stream_, req);

        beast::flat_buffer buffer;
        http::response<http::dynamic_body> res;
        http::read(*stream_, buffer, res);

        beast::get_lowest_layer(*stream_).expires_never();

        if (res.result_int() < 200 || res.result_int() >= 300) {
            std::cerr << "[Sentinel_CloudPublisher] RTDB returned HTTP "
                      << res.result_int() << " - check DB rules\n";
            // Not necessarily a dead connection (could be a 401 on a healthy
            // socket), but safest to reconnect cleanly next time.
            reset_connection();
            return false;
        }

        return true;
    } catch (const std::exception& e) {
        std::cerr << "[Sentinel_CloudPublisher] publish failed: " << e.what() << "\n";
        reset_connection();
        return false;
    }
}

} // namespace sentinel
