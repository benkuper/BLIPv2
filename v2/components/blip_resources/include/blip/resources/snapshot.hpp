#pragma once

#include "blip/resources/board_manifest.hpp"

#include <charconv>
#include <string_view>

namespace blip::resources {

class SnapshotSink {
  public:
    virtual ~SnapshotSink() = default;
    [[nodiscard]] virtual bool write(std::string_view text) noexcept = 0;
};

namespace detail {
inline bool quoted(SnapshotSink& sink, std::string_view value) noexcept {
    if (!sink.write("\"")) return false;
    for (const char ch : value) {
        switch (ch) {
        case '\"':
            if (!sink.write("\\\"")) return false;
            break;
        case '\\':
            if (!sink.write("\\\\")) return false;
            break;
        case '\n':
            if (!sink.write("\\n")) return false;
            break;
        case '\r':
            if (!sink.write("\\r")) return false;
            break;
        case '\t':
            if (!sink.write("\\t")) return false;
            break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20U || !sink.write({&ch, 1U})) return false;
        }
    }
    return sink.write("\"");
}

template <typename Number> bool number(SnapshotSink& sink, Number value) noexcept {
    char buffer[32]{};
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    return result.ec == std::errc{} && sink.write({buffer, static_cast<std::size_t>(result.ptr - buffer)});
}

inline std::string_view mode_name(core::OwnershipMode mode) noexcept {
    switch (mode) {
    case core::OwnershipMode::exclusive: return "exclusive";
    case core::OwnershipMode::shared_read: return "shared-read";
    case core::OwnershipMode::bus_member: return "bus-member";
    case core::OwnershipMode::multiplexed: return "multiplexed";
    }
    return "unknown";
}
} // namespace detail

[[nodiscard]] inline core::Status write_resource_snapshot(const DeviceBroker& broker,
                                                          const BoardManifest& board,
                                                          SnapshotSink& sink) noexcept {
    using detail::number;
    using detail::quoted;
    if (!sink.write("{\"schema_version\":1,\"allocation_revision\":") ||
        !number(sink, broker.revision()) || !sink.write(",\"board\":{") ||
        !sink.write("\"id\":") || !quoted(sink, board.id) || !sink.write(",\"target\":") ||
        !quoted(sink, board.target) || !sink.write(",\"antenna\":") ||
        !quoted(sink, board.antenna) || !sink.write("},\"pins\":[")) {
        return core::Status::failure({core::ErrorDomain::resource,
                                      core::ErrorCode::serialization_overflow, {},
                                      "resource.snapshot", "header"});
    }
    bool first_pin = true;
    bool ok = true;
    broker.visit_resources([&](const ResourceSpec& pin) {
        if (!ok || pin.resource_class != core::ResourceClass::gpio) return;
        if (!first_pin && !sink.write(",")) { ok = false; return; }
        first_pin = false;
        if (!sink.write("{\"id\":") || !quoted(sink, pin.id) || !sink.write(",\"label\":") ||
            !quoted(sink, pin.label) || !sink.write(",\"gpio\":") || !number(sink, pin.gpio) ||
            !sink.write(",\"capabilities\":") || !number(sink, pin.capabilities) ||
            !sink.write(",\"electrical\":") || !quoted(sink, pin.electrical) ||
            !sink.write(",\"selectable\":") || !sink.write(pin.selectable ? "true" : "false") ||
            !sink.write(",\"state\":")) { ok = false; return; }
        bool has_claim = false;
        bool shared = false;
        broker.visit_claims([&](const ClaimView& claim) {
            if (claim.resource_id == pin.id) {
                has_claim = true;
                shared = shared || claim.mode != core::OwnershipMode::exclusive;
            }
        });
        const auto state = !pin.reserved_for.empty() ? std::string_view{"reserved"}
                           : !has_claim             ? std::string_view{"free"}
                           : shared                 ? std::string_view{"shared"}
                                                    : std::string_view{"exclusive"};
        if (!quoted(sink, state) || !sink.write(",\"reason\":") || !quoted(sink, pin.reason) ||
            !sink.write(",\"bus\":") || !quoted(sink, pin.bus) || !sink.write(",\"owners\":[")) {
            ok = false; return;
        }
        bool first_owner = true;
        if (!pin.reserved_for.empty()) {
            ok = sink.write("{\"path\":") && quoted(sink, pin.reserved_for) &&
                 sink.write(",\"role\":") && quoted(sink, pin.reason) &&
                 sink.write(",\"mode\":\"reserved\",\"member_key\":0}");
            first_owner = false;
        }
        broker.visit_claims([&](const ClaimView& claim) {
            if (!ok || claim.resource_id != pin.id) return;
            if (!first_owner && !sink.write(",")) { ok = false; return; }
            first_owner = false;
            ok = sink.write("{\"path\":") && quoted(sink, claim.owner) &&
                 sink.write(",\"role\":") && quoted(sink, claim.role) &&
                 sink.write(",\"mode\":") && quoted(sink, detail::mode_name(claim.mode)) &&
                 sink.write(",\"member_key\":") && number(sink, claim.member_key) &&
                 sink.write(",\"configurable\":true,\"optional\":") &&
                 sink.write(claim.setting_optional ? "true" : "false") &&
                 sink.write(",\"reboot_required\":") &&
                 sink.write(claim.reboot_required ? "true" : "false") && sink.write("}");
        });
        ok = ok && sink.write("],\"configured_owners\":[");
        first_owner = true;
        broker.visit_claims([&](const ClaimView& claim) {
            if (!ok || claim.resource_id != pin.id) return;
            if (!first_owner && !sink.write(",")) { ok = false; return; }
            first_owner = false;
            ok = quoted(sink, claim.owner);
        });
        ok = ok && sink.write("]}");
    });
    if (!ok || !sink.write("]}")) {
        return core::Status::failure({core::ErrorDomain::resource,
                                      core::ErrorCode::serialization_overflow, {},
                                      "resource.snapshot", "body"});
    }
    return core::Status::success();
}

} // namespace blip::resources
