#include "blip/wasm/utf8.hpp"
#include <array>
#include <cstring>

namespace blip::wasm {
namespace {
core::Status failure(core::ErrorCode code, std::string_view detail) noexcept {
    return core::Status::failure({core::ErrorDomain::control, code, "blip.wasm", "utf8", detail});
}
}
bool valid_utf8(std::span<const std::byte> bytes) noexcept {
    std::size_t cursor{};
    while (cursor < bytes.size()) {
        const auto lead = std::to_integer<unsigned>(bytes[cursor++]);
        if (lead < 0x80) continue;
        unsigned following{};
        if (lead >= 0xc2 && lead <= 0xdf) following = 1;
        else if (lead >= 0xe0 && lead <= 0xef) following = 2;
        else if (lead >= 0xf0 && lead <= 0xf4) following = 3;
        else return false;
        if (following > bytes.size() - cursor) return false;
        const auto second = std::to_integer<unsigned>(bytes[cursor]);
        // Shortest forms, exclusion of UTF-16 surrogates, and U+10FFFF ceiling.
        if ((lead == 0xe0 && second < 0xa0) || (lead == 0xed && second > 0x9f) ||
            (lead == 0xf0 && second < 0x90) || (lead == 0xf4 && second > 0x8f)) return false;
        for (unsigned i = 0; i < following; ++i)
            if ((std::to_integer<unsigned>(bytes[cursor++]) & 0xc0U) != 0x80U) return false;
    }
    return true;
}
core::Status read_utf8(GuestMemory& memory, StringRef ref, std::span<char> output, std::size_t& count) noexcept {
    count = 0;
    if (ref.bytes > kMaximumStringBytes || ref.bytes > output.size())
        return failure(core::ErrorCode::capacity_exceeded, "string-or-output-size");
    std::array<std::byte, kMaximumStringBytes> scratch{};
    const auto bytes = std::span(scratch).first(ref.bytes);
    const auto status = memory.read_memory(ref.offset, bytes);
    if (!status) return status;
    if (!valid_utf8(bytes)) return failure(core::ErrorCode::invalid_argument, "invalid-utf8");
    if (!bytes.empty()) std::memcpy(output.data(), bytes.data(), bytes.size());
    count = bytes.size();
    return core::Status::success();
}
core::Status write_utf8(GuestMemory& memory, std::uint32_t offset, std::string_view input) noexcept {
    if (input.size() > kMaximumStringBytes) return failure(core::ErrorCode::capacity_exceeded, "string-size");
    const auto bytes = std::as_bytes(std::span(input.data(), input.size()));
    if (!valid_utf8(bytes)) return failure(core::ErrorCode::invalid_argument, "invalid-utf8");
    return memory.write_memory(offset, bytes);
}
} // namespace blip::wasm
