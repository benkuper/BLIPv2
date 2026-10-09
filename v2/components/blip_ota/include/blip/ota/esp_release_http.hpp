#pragma once
#include "blip/core/error.hpp"
#include "esp_http_client.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

namespace blip::ota {
// Single-worker bounded streaming connection. HTTP is the normal show-network
// transport; HTTPS is available only when explicitly included in the build.
class EspReleaseHttp final {
  public:
    EspReleaseHttp() noexcept = default;
    ~EspReleaseHttp();
    EspReleaseHttp(const EspReleaseHttp&) = delete;
    EspReleaseHttp& operator=(const EspReleaseHttp&) = delete;
    [[nodiscard]] core::Status open(const char* url, std::uint32_t maximum_bytes,
                                    std::uint32_t expected_bytes = 0,
                                    const char* trust_pem = nullptr) noexcept;
    [[nodiscard]] core::Result<std::size_t> read(std::span<std::byte>, const std::atomic<bool>& cancelled) noexcept;
    void close() noexcept;
    [[nodiscard]] int http_status() const noexcept { return http_status_; }
    [[nodiscard]] std::int64_t body_length() const noexcept { return body_length_; }
    [[nodiscard]] std::uint32_t received() const noexcept { return received_; }
  private:
    esp_http_client_handle_t client_{};
    std::int64_t deadline_us_{};
    std::uint32_t maximum_{}, expected_{}, received_{};
    int http_status_{};
    std::int64_t body_length_{};
};
} // namespace blip::ota
