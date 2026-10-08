#include "blip/ota/release_catalog.hpp"
#include <charconv>
#include <limits>

namespace blip::ota {
namespace {
core::Status failure(core::ErrorCode code, std::string_view detail) noexcept {
    return core::Status::failure({core::ErrorDomain::storage, code, "blip.updates", "release-catalog", detail});
}
bool alnum(char c) noexcept { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); }
int hex(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
class Parser {
  public:
    explicit Parser(std::string_view text) : text_(text) {}
    bool token(char value) noexcept { space(); if (position_ == text_.size() || text_[position_] != value) return false; ++position_; return true; }
    bool end() noexcept { space(); return position_ == text_.size(); }
    bool number(std::uint32_t& output) noexcept {
        space(); const auto begin = position_;
        while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') ++position_;
        if (begin == position_ || (position_ - begin > 1 && text_[begin] == '0')) return false;
        const auto result = std::from_chars(text_.data() + begin, text_.data() + position_, output);
        return result.ec == std::errc{} && result.ptr == text_.data() + position_;
    }
    template <std::size_t Capacity> bool string(ReleaseText<Capacity>& output) noexcept {
        if (!token('"')) return false;
        output.length = 0;
        while (position_ < text_.size()) {
            unsigned c = static_cast<unsigned char>(text_[position_++]);
            if (c == '"') { output.bytes[output.length] = '\0'; return true; }
            if (c == '\\') {
                if (position_ == text_.size()) return false;
                c = static_cast<unsigned char>(text_[position_++]);
                if (c == 'u') {
                    if (text_.size() - position_ < 4) return false;
                    c = 0;
                    for (unsigned i = 0; i < 4; ++i) {
                        const auto digit = hex(text_[position_++]); if (digit < 0) return false;
                        c = c * 16 + static_cast<unsigned>(digit);
                    }
                } else if (c != '"' && c != '\\' && c != '/') return false;
            }
            // Catalog identifiers, versions and percent-encoded URLs are ASCII.
            // Reject controls, NUL, Unicode ambiguities and unbounded fields.
            if (c < 0x20 || c >= 0x7f || output.length == Capacity) return false;
            output.bytes[output.length++] = static_cast<char>(c);
        }
        return false;
    }
    bool artifact(ReleaseArtifact& artifact) noexcept {
        space();
        if (text_.substr(position_, 4) == "null") { position_ += 4; artifact = {}; return true; }
        if (!token('{')) return false;
        std::uint32_t seen{};
        constexpr std::array<std::string_view, 6> keys{"code", "version", "bytes", "url", "sha256", "minimum_other_code"};
        for (;;) {
            ReleaseText<31> key;
            if (!string(key) || !token(':')) return false;
            const auto found = std::find(keys.begin(), keys.end(), key.view());
            if (found == keys.end()) return false;
            const auto index = static_cast<unsigned>(found - keys.begin());
            if (seen & (1U << index)) return false;
            seen |= 1U << index;
            bool ok{};
            switch (index) {
            case 0: ok = number(artifact.code); break;
            case 1: ok = string(artifact.version); break;
            case 2: ok = number(artifact.bytes); break;
            case 3: ok = string(artifact.url); break;
            case 4: {
                ReleaseText<64> digest;
                if (!string(digest)) return false;
                const auto parsed = parse_sha256_hex(digest.view());
                if (!parsed) return false;
                artifact.sha256 = parsed.value(); ok = true; break;
            }
            case 5: ok = number(artifact.minimum_other_code); break;
            default: return false;
            }
            if (!ok) return false;
            if (token('}')) break;
            if (!token(',')) return false;
        }
        if (seen != 63) return false;
        artifact.present = true; return true;
    }
    bool catalog(ReleaseCatalog& catalog) noexcept {
        if (!token('{')) return false;
        constexpr std::array<std::string_view, 12> keys{"schema", "project", "board", "target", "layout", "profile",
            "channel", "flash_bytes", "features", "api", "firmware", "web"};
        std::uint32_t seen{};
        for (;;) {
            ReleaseText<31> key;
            if (!string(key) || !token(':')) return false;
            const auto found = std::find(keys.begin(), keys.end(), key.view());
            if (found == keys.end()) return false;
            const auto index = static_cast<unsigned>(found - keys.begin());
            if (seen & (1U << index)) return false;
            seen |= 1U << index;
            bool ok{};
            switch (index) {
            case 0: { std::uint32_t schema{}; ok = number(schema) && schema == 1; break; }
            case 1: ok = string(catalog.project); break;
            case 2: ok = string(catalog.board); break;
            case 3: ok = string(catalog.target); break;
            case 4: ok = string(catalog.layout); break;
            case 5: ok = string(catalog.profile); break;
            case 6: ok = string(catalog.channel); break;
            case 7: ok = number(catalog.flash_bytes); break;
            case 8: ok = number(catalog.features); break;
            case 9: ok = number(catalog.api); break;
            case 10: ok = artifact(catalog.firmware); break;
            case 11: ok = artifact(catalog.web); break;
            default: return false;
            }
            if (!ok) return false;
            if (token('}')) break;
            if (!token(',')) return false;
        }
        return seen == 4095 && end();
    }
  private:
    void space() noexcept {
        while (position_ < text_.size() && (text_[position_] == ' ' || text_[position_] == '\r' || text_[position_] == '\n' || text_[position_] == '\t')) ++position_;
    }
    std::string_view text_; std::size_t position_{};
};
class QueryWriter {
  public:
    explicit QueryWriter(std::span<char> buffer) : buffer_(buffer) {}
    bool append(std::string_view text) noexcept {
        if (buffer_.empty() || text.size() >= buffer_.size() - size_) return false;
        std::copy(text.begin(), text.end(), buffer_.begin() + size_); size_ += text.size(); buffer_[size_] = '\0'; return true;
    }
    bool field(std::string_view name, std::string_view value) noexcept {
        if (!append("&") || !append(name) || !append("=")) return false;
        constexpr char digits[] = "0123456789ABCDEF";
        for (const unsigned char c : value) {
            const char direct = static_cast<char>(c);
            if (alnum(direct) || c == '-' || c == '_' || c == '.' || c == '~') {
                if (!append({&direct, 1})) return false;
            } else {
                const std::array escaped{'%', digits[c >> 4], digits[c & 15]};
                if (!append({escaped.data(), escaped.size()})) return false;
            }
        }
        return true;
    }
    bool field(std::string_view name, std::uint32_t value) noexcept {
        std::array<char, 16> text{}; const auto result = std::to_chars(text.data(), text.data() + text.size(), value);
        return result.ec == std::errc{} && field(name, {text.data(), static_cast<std::size_t>(result.ptr - text.data())});
    }
    std::size_t size() const noexcept { return size_; }
  private:
    std::span<char> buffer_; std::size_t size_{};
};
} // namespace

bool valid_release_https_url(std::string_view url) noexcept {
    if (!url.starts_with("https://") || url.size() > 319 || url.find_first_of("@#\\") != url.npos) return false;
    for (std::size_t i = 0; i < url.size(); ++i) {
        const auto c = static_cast<unsigned char>(url[i]);
        if (c <= 0x20 || c >= 0x7f) return false;
        if (c == '%') { if (url.size() - i < 3 || hex(url[i + 1]) < 0 || hex(url[i + 2]) < 0) return false; i += 2; }
    }
    const auto remainder = url.substr(8);
    const auto authority = remainder.substr(0, remainder.find_first_of("/?"));
    auto host = authority;
    const auto colon = authority.find(':');
    if (colon != authority.npos) {
        host = authority.substr(0, colon);
        const auto port = authority.substr(colon + 1); std::uint32_t number{};
        const auto parsed = std::from_chars(port.data(), port.data() + port.size(), number);
        if (port.empty() || parsed.ec != std::errc{} || parsed.ptr != port.data() + port.size() || !number || number > 65535) return false;
    }
    if (host.empty() || host.size() > 253) return false;
    while (!host.empty()) {
        const auto dot = host.find('.'); const auto label = host.substr(0, dot);
        if (label.empty() || label.size() > 63 || !alnum(label.front()) || !alnum(label.back())) return false;
        for (const char c : label) if (!alnum(c) && c != '-') return false;
        if (dot == host.npos) break;
        host.remove_prefix(dot + 1); if (host.empty()) return false;
    }
    return true;
}
core::Status decode_release_catalog(std::string_view json, ReleaseCatalog& output) noexcept {
    output = {};
    if (json.empty() || json.size() > kMaximumReleaseCatalogBytes || !Parser(json).catalog(output)) {
        output = {}; return failure(core::ErrorCode::corrupt_data, "invalid-bounded-release-json");
    }
    return core::Status::success();
}
core::Status validate_release_catalog(const ReleaseCatalog& catalog, const ReleaseIdentity& identity,
                                    std::uint32_t maximum_firmware_bytes, std::uint32_t maximum_web_bytes) noexcept {
    if (catalog.project.view().empty() || catalog.board.view().empty() || catalog.target.view().empty() ||
        catalog.layout.view().empty() || catalog.profile.view().empty() || catalog.channel.view().empty() || !catalog.api ||
        catalog.project.view() != identity.project || catalog.board.view() != identity.board || catalog.target.view() != identity.target ||
        catalog.layout.view() != identity.layout || catalog.profile.view() != identity.profile || catalog.channel.view() != identity.channel ||
        catalog.flash_bytes != identity.flash_bytes || catalog.features != identity.features || catalog.api != identity.api)
        return failure(core::ErrorCode::incompatible_version, "device-release-identity-mismatch");
    for (const auto* artifact : {&catalog.firmware, &catalog.web}) {
        if (!artifact->present) continue;
        const bool firmware = artifact == &catalog.firmware;
        if (!artifact->code || artifact->version.view().empty() || !valid_release_https_url(artifact->url.view()) ||
            artifact->bytes < (firmware ? kEspAppDescriptorEnd : 48) ||
            artifact->bytes > (firmware ? maximum_firmware_bytes : maximum_web_bytes) ||
            std::all_of(artifact->sha256.begin(), artifact->sha256.end(), [](std::byte byte) { return byte == std::byte{}; }))
            return failure(core::ErrorCode::verification_failed, "invalid-release-artifact");
    }
    return core::Status::success();
}
bool firmware_update_available(const ReleaseCatalog& catalog, const ReleaseIdentity& identity) noexcept {
    return catalog.firmware.present && catalog.firmware.code > identity.firmware_code && catalog.firmware.minimum_other_code <= identity.web_code;
}
bool web_update_available(const ReleaseCatalog& catalog, const ReleaseIdentity& identity) noexcept {
    return catalog.web.present && catalog.web.code > identity.web_code && catalog.web.minimum_other_code <= identity.firmware_code;
}
core::Status build_release_query(std::string_view endpoint, const ReleaseIdentity& identity,
                                std::span<char> output, std::size_t& written) noexcept {
    written = 0; if (!output.empty()) output.front() = '\0';
    const auto text_valid = [](std::string_view text, std::size_t capacity) {
        return !text.empty() && text.size() <= capacity && std::all_of(text.begin(), text.end(), [](char c) {
            const auto byte = static_cast<unsigned char>(c); return byte >= 0x20 && byte < 0x7f;
        });
    };
    if (!valid_release_https_url(endpoint) || !text_valid(identity.project, 31) || !text_valid(identity.board, 63) ||
        !text_valid(identity.target, 15) || !text_valid(identity.layout, 31) || !text_valid(identity.profile, 31) ||
        !text_valid(identity.channel, 15) || !text_valid(identity.firmware_version, 31) || !identity.api || !identity.flash_bytes)
        return failure(core::ErrorCode::invalid_argument, "invalid-release-query-identity");
    QueryWriter writer(output);
    const auto separator = endpoint.ends_with('?') || endpoint.ends_with('&') ? "" : endpoint.find('?') == endpoint.npos ? "?" : "&";
    if (!writer.append(endpoint) || !writer.append(separator) || !writer.append("schema=1") ||
        !writer.field("project", identity.project) || !writer.field("board", identity.board) || !writer.field("target", identity.target) ||
        !writer.field("layout", identity.layout) || !writer.field("profile", identity.profile) || !writer.field("channel", identity.channel) ||
        !writer.field("flash_bytes", identity.flash_bytes) || !writer.field("features", identity.features) || !writer.field("api", identity.api) ||
        !writer.field("fw_code", identity.firmware_code) || !writer.field("fw_version", identity.firmware_version) || !writer.field("web_code", identity.web_code)) {
        if (!output.empty()) output.front() = '\0';
        return failure(core::ErrorCode::capacity_exceeded, "release-query-buffer");
    }
    written = writer.size(); return core::Status::success();
}
} // namespace blip::ota
