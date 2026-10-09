#pragma once

#include "blip/core/error.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <mutex>

namespace blip::storage {

inline constexpr std::uint16_t kWebAssetBundleFormatVersion = 1;
inline constexpr std::size_t kWebAssetBundleHeaderBytes = 48;
inline constexpr std::size_t kWebAssetBundleEntryBytes = 96;
inline constexpr std::size_t kMaxWebAssetPathBytes = 63;
inline constexpr std::size_t kMaxWebAssets = 24;
inline constexpr std::size_t kMaxWebAssetBundleBytes = 256U * 1024U;

enum class WebContentType : std::uint8_t {
    html = 1,
    css = 2,
    javascript = 3,
    json = 4,
    text = 5,
    png = 6,
    icon = 7,
};

enum class WebContentEncoding : std::uint8_t { identity = 0, gzip = 1 };
enum class WebCachePolicy : std::uint8_t { no_store = 0, revalidate = 1, immutable = 2 };

struct WebAsset {
    std::array<char, kMaxWebAssetPathBytes + 1U> path{};
    std::size_t path_size{};
    WebContentType content_type{WebContentType::text};
    WebContentEncoding encoding{WebContentEncoding::identity};
    WebCachePolicy cache_policy{WebCachePolicy::no_store};
    std::uint32_t data_offset{};
    std::uint32_t stored_size{};
    std::uint32_t original_size{};
    std::uint32_t crc32{};

    [[nodiscard]] std::string_view path_view() const noexcept { return {path.data(), path_size}; }
};

struct WebAssetBundleInfo {
    std::uint32_t bundle_version{};
    std::uint32_t total_size{};
    std::uint32_t payload_size{};
    std::uint32_t bundle_crc32{};
    std::size_t asset_count{};
};

class WebAssetBackend {
  public:
    WebAssetBackend() = default;
    virtual ~WebAssetBackend() = default;
    WebAssetBackend(const WebAssetBackend&) = delete;
    WebAssetBackend& operator=(const WebAssetBackend&) = delete;
    WebAssetBackend(WebAssetBackend&&) = delete;
    WebAssetBackend& operator=(WebAssetBackend&&) = delete;

    [[nodiscard]] virtual core::Result<std::size_t> file_size(std::string_view path) noexcept = 0;
    [[nodiscard]] virtual core::Result<std::size_t>
    read_at(std::string_view path, std::size_t offset, std::span<std::byte> output) noexcept = 0;
    [[nodiscard]] virtual core::Status begin_write(std::string_view path) noexcept = 0;
    [[nodiscard]] virtual core::Status append_write(std::span<const std::byte> value) noexcept = 0;
    [[nodiscard]] virtual core::Status finish_write() noexcept = 0;
    // Reopen a closed staging file for a small durable commit trailer.
    [[nodiscard]] virtual core::Status resume_write(std::string_view) noexcept {
        return core::Status::failure({core::ErrorDomain::storage, core::ErrorCode::invalid_state,
                                      {}, "file-resume", "unsupported"});
    }
    virtual void abort_write() noexcept = 0;
    [[nodiscard]] virtual core::Status replace(std::string_view source,
                                               std::string_view destination) noexcept = 0;
    [[nodiscard]] virtual core::Status remove(std::string_view path) noexcept = 0;
};

class WebAssetStore {
  public:
    static constexpr std::string_view kActivePath = "web/assets.bundle";
    static constexpr std::string_view kUploadPath = "web/assets.upload";

    WebAssetStore(WebAssetBackend& backend, std::span<std::byte> scratch) noexcept
        : backend_(&backend), scratch_(scratch) {}

    WebAssetStore(const WebAssetStore&) = delete;
    WebAssetStore& operator=(const WebAssetStore&) = delete;
    WebAssetStore(WebAssetStore&&) = delete;
    WebAssetStore& operator=(WebAssetStore&&) = delete;

    [[nodiscard]] core::Result<WebAssetBundleInfo> load_active() noexcept;
    [[nodiscard]] core::Status ensure_factory(std::span<const std::byte> bundle) noexcept;
    [[nodiscard]] core::Status begin_install(std::size_t expected_size) noexcept;
    [[nodiscard]] core::Status append_install(std::span<const std::byte> chunk) noexcept;
    [[nodiscard]] core::Result<WebAssetBundleInfo> finish_install() noexcept;
    void cancel_install() noexcept;

    [[nodiscard]] const WebAsset* find(std::string_view request_path) const noexcept;
    [[nodiscard]] core::Result<std::size_t> read(const WebAsset& asset, std::size_t offset,
                                                 std::span<std::byte> output) noexcept;

    // Hold this lock from find() through the last read() when using the borrowed
    // asset pointer, and across a complete upload to retain transaction ownership.
    [[nodiscard]] std::recursive_mutex& mutex() const noexcept { return mutex_; }
    // Published status never waits for file I/O or candidate validation.
    [[nodiscard]] bool active() const noexcept { std::lock_guard guard(info_mutex_); return active_; }
    [[nodiscard]] WebAssetBundleInfo info() const noexcept { std::lock_guard guard(info_mutex_); return info_; }

  private:
    [[nodiscard]] core::Result<WebAssetBundleInfo> validate(std::string_view storage_path,
                                                            bool load_entries) noexcept;
    [[nodiscard]] core::Result<std::size_t> read_exact(std::string_view path, std::size_t offset,
                                                       std::span<std::byte> output) noexcept;
    [[nodiscard]] core::Result<std::uint32_t> crc_range(std::string_view path, std::size_t offset,
                                                        std::size_t size,
                                                        bool zero_bundle_crc) noexcept;
    [[nodiscard]] core::Status install_complete(std::span<const std::byte> bundle) noexcept;

    WebAssetBackend* backend_{};
    mutable std::recursive_mutex mutex_{};
    mutable std::mutex info_mutex_{};
    std::span<std::byte> scratch_{};
    std::array<WebAsset, kMaxWebAssets> entries_{};
    WebAssetBundleInfo info_{};
    std::size_t expected_size_{};
    std::size_t received_size_{};
    bool active_{};
    bool installing_{};
};

[[nodiscard]] constexpr std::string_view content_type_name(WebContentType type) noexcept {
    switch (type) {
    case WebContentType::html:
        return "text/html; charset=utf-8";
    case WebContentType::css:
        return "text/css; charset=utf-8";
    case WebContentType::javascript:
        return "text/javascript; charset=utf-8";
    case WebContentType::json:
        return "application/json; charset=utf-8";
    case WebContentType::text:
        return "text/plain; charset=utf-8";
    case WebContentType::png:
        return "image/png";
    case WebContentType::icon:
        return "image/x-icon";
    }
    return "application/octet-stream";
}

[[nodiscard]] constexpr std::string_view cache_control_name(WebCachePolicy policy) noexcept {
    switch (policy) {
    case WebCachePolicy::no_store:
        return "no-store";
    case WebCachePolicy::revalidate:
        return "no-cache";
    case WebCachePolicy::immutable:
        return "public, max-age=31536000, immutable";
    }
    return "no-store";
}

} // namespace blip::storage
