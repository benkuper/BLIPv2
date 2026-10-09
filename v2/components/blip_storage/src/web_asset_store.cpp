#include "blip/storage/web_asset_store.hpp"

#include <algorithm>
#include <cstring>

namespace blip::storage {
namespace {

constexpr std::array<std::byte, 4> kMagic{std::byte{0x42}, std::byte{0x4c}, std::byte{0x57},
                                          std::byte{0x42}};
constexpr std::size_t kBundleCrcOffset = 28;
constexpr std::size_t kPathOffset = 24;
constexpr std::size_t kPathFieldBytes = 64;

[[nodiscard]] core::Error asset_error(core::ErrorCode code, std::string_view operation,
                                      std::string_view detail) noexcept {
    return {core::ErrorDomain::storage, code, "blip.storage.files.internal", operation, detail};
}

[[nodiscard]] std::uint16_t u16(std::span<const std::byte> value, std::size_t offset) noexcept {
    return std::to_integer<std::uint16_t>(value[offset]) |
           (std::to_integer<std::uint16_t>(value[offset + 1U]) << 8U);
}

[[nodiscard]] std::uint32_t u32(std::span<const std::byte> value, std::size_t offset) noexcept {
    return std::to_integer<std::uint32_t>(value[offset]) |
           (std::to_integer<std::uint32_t>(value[offset + 1U]) << 8U) |
           (std::to_integer<std::uint32_t>(value[offset + 2U]) << 16U) |
           (std::to_integer<std::uint32_t>(value[offset + 3U]) << 24U);
}

[[nodiscard]] std::uint32_t crc_update(std::uint32_t crc,
                                       std::span<const std::byte> value) noexcept {
    for (const std::byte byte : value) {
        crc ^= std::to_integer<std::uint8_t>(byte);
        for (std::size_t bit = 0; bit < 8U; ++bit) {
            const std::uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0xedb88320U & mask);
        }
    }
    return crc;
}

[[nodiscard]] bool valid_path(std::string_view path) noexcept {
    if (path.empty() || path.size() > kMaxWebAssetPathBytes || path.front() != '/' || path == "/" ||
        path.find("..") != std::string_view::npos || path.find("//") != std::string_view::npos) {
        return false;
    }
    return std::all_of(path.begin(), path.end(), [](char value) {
        return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
               (value >= '0' && value <= '9') || value == '/' || value == '-' || value == '_' ||
               value == '.';
    });
}

[[nodiscard]] bool zero(std::span<const std::byte> value) noexcept {
    return std::all_of(value.begin(), value.end(),
                       [](std::byte byte) { return byte == std::byte{}; });
}

} // namespace

core::Result<std::size_t> WebAssetStore::read_exact(std::string_view path, std::size_t offset,
                                                    std::span<std::byte> output) noexcept {
    const auto result = backend_->read_at(path, offset, output);
    if (!result) {
        return result;
    }
    if (result.value() != output.size()) {
        return core::Result<std::size_t>::failure(
            asset_error(core::ErrorCode::corrupt_data, "bundle-read", "truncated"));
    }
    return result;
}

core::Result<std::uint32_t> WebAssetStore::crc_range(std::string_view path, std::size_t offset,
                                                     std::size_t size,
                                                     bool zero_bundle_crc) noexcept {
    if (scratch_.empty()) {
        return core::Result<std::uint32_t>::failure(
            asset_error(core::ErrorCode::capacity_exceeded, "bundle-crc", "scratch-empty"));
    }
    std::uint32_t crc = 0xffffffffU;
    std::size_t processed{};
    while (processed < size) {
        const std::size_t count = std::min(scratch_.size(), size - processed);
        auto chunk = scratch_.first(count);
        const auto read_status = read_exact(path, offset + processed, chunk);
        if (!read_status) {
            return core::Result<std::uint32_t>::failure(read_status.error());
        }
        if (zero_bundle_crc) {
            for (std::size_t index = 0; index < count; ++index) {
                const std::size_t absolute = offset + processed + index;
                if (absolute >= kBundleCrcOffset && absolute < kBundleCrcOffset + 4U) {
                    chunk[index] = std::byte{};
                }
            }
        }
        crc = crc_update(crc, chunk);
        processed += count;
    }
    return core::Result<std::uint32_t>::success(crc ^ 0xffffffffU);
}

core::Result<WebAssetBundleInfo> WebAssetStore::validate(std::string_view storage_path,
                                                         bool load_entries) noexcept {
    if (scratch_.size() < kWebAssetBundleEntryBytes) {
        return core::Result<WebAssetBundleInfo>::failure(
            asset_error(core::ErrorCode::capacity_exceeded, "bundle-validate", "scratch-small"));
    }
    const auto size_result = backend_->file_size(storage_path);
    if (!size_result) {
        return core::Result<WebAssetBundleInfo>::failure(size_result.error());
    }
    const std::size_t file_size = size_result.value();
    if (file_size < kWebAssetBundleHeaderBytes || file_size > kMaxWebAssetBundleBytes) {
        return core::Result<WebAssetBundleInfo>::failure(
            asset_error(core::ErrorCode::corrupt_data, "bundle-validate", "file-size"));
    }
    auto header = scratch_.first(kWebAssetBundleHeaderBytes);
    const auto header_status = read_exact(storage_path, 0, header);
    if (!header_status) {
        return core::Result<WebAssetBundleInfo>::failure(header_status.error());
    }
    const bool magic = std::equal(kMagic.begin(), kMagic.end(), header.begin());
    if (!magic || u16(header, 4) != kWebAssetBundleFormatVersion ||
        u16(header, 6) != kWebAssetBundleHeaderBytes) {
        return core::Result<WebAssetBundleInfo>::failure(asset_error(
            magic ? core::ErrorCode::incompatible_version : core::ErrorCode::corrupt_data,
            "bundle-validate", magic ? "format-version" : "magic"));
    }
    const std::uint32_t bundle_version = u32(header, 8);
    const std::size_t entry_count = u16(header, 12);
    const std::size_t manifest_size = u32(header, 16);
    const std::size_t payload_size = u32(header, 20);
    const std::size_t total_size = u32(header, 24);
    const std::uint32_t expected_bundle_crc = u32(header, 28);
    const std::uint32_t expected_manifest_crc = u32(header, 32);
    const std::uint32_t expected_payload_crc = u32(header, 36);
    if (bundle_version == 0U || entry_count == 0U || entry_count > kMaxWebAssets ||
        u16(header, 14) != 0U || manifest_size != entry_count * kWebAssetBundleEntryBytes ||
        total_size != file_size ||
        total_size != kWebAssetBundleHeaderBytes + manifest_size + payload_size ||
        !zero(header.subspan(40, 8))) {
        return core::Result<WebAssetBundleInfo>::failure(
            asset_error(core::ErrorCode::corrupt_data, "bundle-validate", "header"));
    }
    const auto bundle_crc = crc_range(storage_path, 0, total_size, true);
    const auto manifest_crc =
        crc_range(storage_path, kWebAssetBundleHeaderBytes, manifest_size, false);
    const auto payload_crc =
        crc_range(storage_path, kWebAssetBundleHeaderBytes + manifest_size, payload_size, false);
    if (!bundle_crc || !manifest_crc || !payload_crc) {
        const auto& error = !bundle_crc     ? bundle_crc.error()
                            : !manifest_crc ? manifest_crc.error()
                                            : payload_crc.error();
        return core::Result<WebAssetBundleInfo>::failure(error);
    }
    if (bundle_crc.value() != expected_bundle_crc ||
        manifest_crc.value() != expected_manifest_crc ||
        payload_crc.value() != expected_payload_crc) {
        return core::Result<WebAssetBundleInfo>::failure(
            asset_error(core::ErrorCode::verification_failed, "bundle-validate", "crc"));
    }

    std::array<char, kMaxWebAssetPathBytes + 1U> previous{};
    std::size_t previous_size{};
    std::uint32_t expected_offset{};
    bool has_index{};
    for (std::size_t index = 0; index < entry_count; ++index) {
        auto record = scratch_.first(kWebAssetBundleEntryBytes);
        const std::size_t record_offset = kWebAssetBundleHeaderBytes + index * record.size();
        const auto record_status = read_exact(storage_path, record_offset, record);
        if (!record_status) {
            return core::Result<WebAssetBundleInfo>::failure(record_status.error());
        }
        const std::size_t path_size = std::to_integer<std::uint8_t>(record[0]);
        const auto content_type =
            static_cast<WebContentType>(std::to_integer<std::uint8_t>(record[1]));
        const auto encoding =
            static_cast<WebContentEncoding>(std::to_integer<std::uint8_t>(record[2]));
        const auto cache_policy =
            static_cast<WebCachePolicy>(std::to_integer<std::uint8_t>(record[3]));
        const std::uint32_t data_offset = u32(record, 4);
        const std::uint32_t stored_size = u32(record, 8);
        const std::uint32_t original_size = u32(record, 12);
        const std::uint32_t asset_crc = u32(record, 16);
        if (path_size == 0U || path_size > kMaxWebAssetPathBytes) {
            return core::Result<WebAssetBundleInfo>::failure(
                asset_error(core::ErrorCode::corrupt_data, "bundle-validate", "entry-path"));
        }
        std::array<char, kMaxWebAssetPathBytes + 1U> current_path{};
        std::memcpy(current_path.data(), record.data() + kPathOffset, path_size);
        const std::string_view path{current_path.data(), path_size};
        if (!valid_path(path)) {
            return core::Result<WebAssetBundleInfo>::failure(
                asset_error(core::ErrorCode::corrupt_data, "bundle-validate", "entry-path"));
        }
        if (data_offset != expected_offset || stored_size == 0U ||
            static_cast<std::uint64_t>(data_offset) + stored_size > payload_size) {
            return core::Result<WebAssetBundleInfo>::failure(
                asset_error(core::ErrorCode::corrupt_data, "bundle-validate", "entry-layout"));
        }
        if (static_cast<std::uint8_t>(content_type) <
                static_cast<std::uint8_t>(WebContentType::html) ||
            static_cast<std::uint8_t>(content_type) >
                static_cast<std::uint8_t>(WebContentType::icon) ||
            static_cast<std::uint8_t>(encoding) >
                static_cast<std::uint8_t>(WebContentEncoding::gzip) ||
            static_cast<std::uint8_t>(cache_policy) >
                static_cast<std::uint8_t>(WebCachePolicy::immutable)) {
            return core::Result<WebAssetBundleInfo>::failure(
                asset_error(core::ErrorCode::corrupt_data, "bundle-validate", "entry-enum"));
        }
        if (!zero(record.subspan(20, 4)) ||
            !zero(record.subspan(kPathOffset + path_size, kPathFieldBytes - path_size)) ||
            !zero(record.subspan(88, 8))) {
            return core::Result<WebAssetBundleInfo>::failure(
                asset_error(core::ErrorCode::corrupt_data, "bundle-validate", "entry-reserved"));
        }
        if (index != 0U && std::string_view{previous.data(), previous_size} >= path) {
            return core::Result<WebAssetBundleInfo>::failure(
                asset_error(core::ErrorCode::corrupt_data, "bundle-validate", "entry-order"));
        }
        const auto actual_asset_crc =
            crc_range(storage_path, kWebAssetBundleHeaderBytes + manifest_size + data_offset,
                      stored_size, false);
        if (!actual_asset_crc || actual_asset_crc.value() != asset_crc) {
            return core::Result<WebAssetBundleInfo>::failure(
                actual_asset_crc ? asset_error(core::ErrorCode::verification_failed,
                                               "bundle-validate", "asset-crc")
                                 : actual_asset_crc.error());
        }
        std::memcpy(previous.data(), path.data(), path.size());
        previous[path.size()] = '\0';
        previous_size = path.size();
        has_index = has_index || path == "/index.html";
        expected_offset += stored_size;
        if (load_entries) {
            auto& entry = entries_[index];
            entry = {};
            std::memcpy(entry.path.data(), path.data(), path.size());
            entry.path[path.size()] = '\0';
            entry.path_size = path.size();
            entry.content_type = content_type;
            entry.encoding = encoding;
            entry.cache_policy = cache_policy;
            entry.data_offset = static_cast<std::uint32_t>(kWebAssetBundleHeaderBytes +
                                                           manifest_size + data_offset);
            entry.stored_size = stored_size;
            entry.original_size = original_size;
            entry.crc32 = asset_crc;
        }
    }
    if (!has_index || expected_offset != payload_size) {
        return core::Result<WebAssetBundleInfo>::failure(
            asset_error(core::ErrorCode::corrupt_data, "bundle-validate", "payload-layout"));
    }
    return core::Result<WebAssetBundleInfo>::success(
        {bundle_version, static_cast<std::uint32_t>(total_size),
         static_cast<std::uint32_t>(payload_size), expected_bundle_crc, entry_count});
}

core::Result<WebAssetBundleInfo> WebAssetStore::load_active() noexcept {
    std::lock_guard guard(mutex_);
    active_ = false;
    info_ = {};
    const auto result = validate(kActivePath, true);
    if (!result) {
        return result;
    }
    info_ = result.value();
    active_ = true;
    return result;
}

core::Status WebAssetStore::begin_install(std::size_t expected_size) noexcept {
    std::lock_guard guard(mutex_);
    if (installing_ || expected_size < kWebAssetBundleHeaderBytes ||
        expected_size > kMaxWebAssetBundleBytes) {
        return core::Status::failure(asset_error(
            installing_ ? core::ErrorCode::invalid_state : core::ErrorCode::capacity_exceeded,
            "bundle-begin", installing_ ? "install-active" : "bundle-size"));
    }
    static_cast<void>(backend_->remove(kUploadPath));
    const auto status = backend_->begin_write(kUploadPath);
    if (!status) {
        return status;
    }
    expected_size_ = expected_size;
    received_size_ = 0;
    installing_ = true;
    return core::Status::success();
}

core::Status WebAssetStore::append_install(std::span<const std::byte> chunk) noexcept {
    std::lock_guard guard(mutex_);
    if (!installing_ || chunk.empty() || received_size_ + chunk.size() > expected_size_) {
        return core::Status::failure(asset_error(core::ErrorCode::invalid_state, "bundle-append",
                                                 installing_ ? "size-mismatch" : "not-started"));
    }
    const auto status = backend_->append_write(chunk);
    if (!status) {
        cancel_install();
        return status;
    }
    received_size_ += chunk.size();
    return core::Status::success();
}

core::Result<WebAssetBundleInfo> WebAssetStore::finish_install() noexcept {
    std::lock_guard guard(mutex_);
    if (!installing_ || received_size_ != expected_size_) {
        cancel_install();
        return core::Result<WebAssetBundleInfo>::failure(
            asset_error(core::ErrorCode::invalid_state, "bundle-finish", "size-mismatch"));
    }
    const auto finish_status = backend_->finish_write();
    installing_ = false;
    if (!finish_status) {
        static_cast<void>(backend_->remove(kUploadPath));
        return core::Result<WebAssetBundleInfo>::failure(finish_status.error());
    }
    const auto candidate = validate(kUploadPath, false);
    if (!candidate) {
        static_cast<void>(backend_->remove(kUploadPath));
        static_cast<void>(load_active());
        return candidate;
    }
    const auto replace_status = backend_->replace(kUploadPath, kActivePath);
    if (!replace_status) {
        static_cast<void>(backend_->remove(kUploadPath));
        const auto recovered = load_active();
        if (recovered && recovered.value().bundle_crc32 == candidate.value().bundle_crc32) {
            return recovered;
        }
        return core::Result<WebAssetBundleInfo>::failure(replace_status.error());
    }
    return load_active();
}

void WebAssetStore::cancel_install() noexcept {
    std::lock_guard guard(mutex_);
    if (installing_) {
        backend_->abort_write();
    }
    installing_ = false;
    expected_size_ = 0;
    received_size_ = 0;
    static_cast<void>(backend_->remove(kUploadPath));
}

core::Status WebAssetStore::install_complete(std::span<const std::byte> bundle) noexcept {
    auto status = begin_install(bundle.size());
    if (!status) {
        return status;
    }
    status = append_install(bundle);
    if (!status) {
        return status;
    }
    const auto installed = finish_install();
    return installed ? core::Status::success() : core::Status::failure(installed.error());
}

core::Status WebAssetStore::ensure_factory(std::span<const std::byte> bundle) noexcept {
    std::lock_guard guard(mutex_);
    const auto cleanup_status = backend_->remove(kUploadPath);
    if (!cleanup_status) {
        return cleanup_status;
    }
    const auto loaded = load_active();
    if (loaded) {
        return core::Status::success();
    }
    if (loaded.error().code != core::ErrorCode::not_found &&
        loaded.error().code != core::ErrorCode::corrupt_data &&
        loaded.error().code != core::ErrorCode::incompatible_version &&
        loaded.error().code != core::ErrorCode::verification_failed) {
        return core::Status::failure(loaded.error());
    }
    if (loaded.error().code != core::ErrorCode::not_found) {
        const auto remove_status = backend_->remove(kActivePath);
        if (!remove_status) {
            return remove_status;
        }
    }
    return install_complete(bundle);
}

const WebAsset* WebAssetStore::find(std::string_view request_path) const noexcept {
    std::lock_guard guard(mutex_);
    if (!active_) {
        return nullptr;
    }
    const std::string_view path =
        request_path == "/" ? std::string_view{"/index.html"} : request_path;
    const auto begin = entries_.begin();
    const auto end = begin + static_cast<std::ptrdiff_t>(info_.asset_count);
    const auto found =
        std::lower_bound(begin, end, path, [](const WebAsset& asset, std::string_view candidate) {
            return asset.path_view() < candidate;
        });
    return found != end && found->path_view() == path ? &*found : nullptr;
}

core::Result<std::size_t> WebAssetStore::read(const WebAsset& asset, std::size_t offset,
                                              std::span<std::byte> output) noexcept {
    std::lock_guard guard(mutex_);
    if (!active_ || offset > asset.stored_size) {
        return core::Result<std::size_t>::failure(
            asset_error(core::ErrorCode::invalid_argument, "asset-read", "offset"));
    }
    const std::size_t stored_size = static_cast<std::size_t>(asset.stored_size);
    const std::size_t count = std::min(output.size(), stored_size - offset);
    if (count == 0U) {
        return core::Result<std::size_t>::success(0U);
    }
    return read_exact(kActivePath, asset.data_offset + offset, output.first(count));
}

} // namespace blip::storage
