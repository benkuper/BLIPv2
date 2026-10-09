#include "blip/ota/esp_release_http.hpp"
#include "sdkconfig.h"
#if defined(CONFIG_ESP_HTTP_CLIENT_ENABLE_HTTPS)
#include "esp_crt_bundle.h"
#endif
#include "esp_timer.h"
#include <algorithm>
#include <cstring>
#include <ctime>
#include <string_view>

namespace blip::ota {
namespace {
core::Status failure(core::ErrorCode code, std::string_view detail) noexcept {
    return core::Status::failure({core::ErrorDomain::transport, code, "blip.updates", "https", detail});
}
}
EspReleaseHttp::~EspReleaseHttp() { close(); }
void EspReleaseHttp::close() noexcept {
    if (client_) {
        static_cast<void>(esp_http_client_close(client_));
        static_cast<void>(esp_http_client_cleanup(client_));
        client_ = nullptr;
    }
}
core::Status EspReleaseHttp::open(const char* url, std::uint32_t maximum_bytes,
                                 std::uint32_t expected_bytes, const char* trust_pem) noexcept {
    close(); received_ = 0; http_status_ = 0; body_length_ = 0;
    const std::size_t length = url ? strnlen(url, 1024) : 0;
    if (!length || length >= 1024 || !maximum_bytes || expected_bytes > maximum_bytes)
        return failure(core::ErrorCode::invalid_argument, "request-bounds");
    const std::string_view text(url, length);
    const bool encrypted = text.starts_with("https://");
    if ((!encrypted && !text.starts_with("http://")) || text.find_first_of("@#\\") != text.npos ||
        std::any_of(text.begin(), text.end(), [](char c) { const auto b = static_cast<unsigned char>(c); return b <= 32 || b >= 127; }))
        return failure(core::ErrorCode::invalid_argument, "http-url-required");
#if defined(CONFIG_ESP_HTTP_CLIENT_ENABLE_HTTPS)
    if (encrypted && std::time(nullptr) < 1704067200)
        return failure(core::ErrorCode::invalid_state, "clock-not-synchronized");
#else
    static_cast<void>(trust_pem);
    if (encrypted) return failure(core::ErrorCode::invalid_state, "https-not-included");
#endif
    maximum_ = maximum_bytes; expected_ = expected_bytes;
    deadline_us_ = esp_timer_get_time() + 120000000;
    esp_http_client_config_t config{};
    config.url = url; config.method = HTTP_METHOD_GET;
    config.transport_type = encrypted ? HTTP_TRANSPORT_OVER_SSL : HTTP_TRANSPORT_OVER_TCP;
    config.timeout_ms = 5000;
    config.buffer_size = 512;
    config.buffer_size_tx = static_cast<int>(std::max<std::size_t>(384, length + 128));
    config.user_agent = "BLIPv2";
    config.disable_auto_redirect = true;
    config.keep_alive_enable = false;
#if defined(CONFIG_ESP_HTTP_CLIENT_ENABLE_HTTPS)
    if (trust_pem) config.cert_pem = trust_pem;
    else config.crt_bundle_attach = esp_crt_bundle_attach;
#endif
    client_ = esp_http_client_init(&config);
    if (!client_) return failure(core::ErrorCode::resource_unavailable, "http-client-allocation");
    if (esp_http_client_set_header(client_, "Accept-Encoding", "identity") != ESP_OK ||
        esp_http_client_set_header(client_, "User-Agent", "BLIPv2") != ESP_OK ||
        esp_http_client_open(client_, 0) != ESP_OK) {
        close(); return failure(core::ErrorCode::io_failed, "https-connect");
    }
    const auto content_length = esp_http_client_fetch_headers(client_);
    body_length_ = content_length;
    http_status_ = esp_http_client_get_status_code(client_);
    if (content_length < 0 || http_status_ != 200) {
        close(); return failure(core::ErrorCode::io_failed, "http-status-or-headers");
    }
    if (content_length > maximum_ || (content_length > 0 && expected_ && content_length != expected_)) {
        close(); return failure(core::ErrorCode::verification_failed, "content-length");
    }
    return core::Status::success();
}
core::Result<std::size_t> EspReleaseHttp::read(std::span<std::byte> output, const std::atomic<bool>& cancelled) noexcept {
    const auto fail = [](core::ErrorCode code, std::string_view detail) {
        return core::Result<std::size_t>::failure(failure(code, detail).error());
    };
    if (!client_ || output.empty()) return fail(core::ErrorCode::invalid_state, "read-state");
    if (cancelled.load()) return fail(core::ErrorCode::cancelled, "request-cancelled");
    if (esp_timer_get_time() >= deadline_us_) return fail(core::ErrorCode::budget_exceeded, "request-deadline");
    const auto count = std::min<std::size_t>({output.size(), 1024, static_cast<std::size_t>(maximum_ - received_) + 1});
    const int read = esp_http_client_read(client_, reinterpret_cast<char*>(output.data()), static_cast<int>(count));
    if (read < 0) return fail(core::ErrorCode::io_failed, "body-read");
    if (read == 0) {
        if (!esp_http_client_is_complete_data_received(client_) || (expected_ && received_ != expected_))
            return fail(core::ErrorCode::verification_failed, "truncated-body");
        return core::Result<std::size_t>::success(0);
    }
    if (static_cast<std::uint32_t>(read) > maximum_ - received_ ||
        (expected_ && static_cast<std::uint32_t>(read) > expected_ - received_))
        return fail(core::ErrorCode::capacity_exceeded, "body-exceeds-bound");
    received_ += static_cast<std::uint32_t>(read);
    return core::Result<std::size_t>::success(static_cast<std::size_t>(read));
}
} // namespace blip::ota
