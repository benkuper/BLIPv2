#include "blip/wasm/module_policy.hpp"
#include "blip/wasm/capability.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>
namespace blip::wasm {
namespace {
class Reader {
  public:
    explicit Reader(std::span<const std::byte> bytes) noexcept : bytes_(bytes) {}
    bool byte(unsigned& out) noexcept {
        if (position_ == bytes_.size()) return false;
        out = std::to_integer<unsigned>(bytes_[position_++]); return true;
    }
    bool u32(std::uint32_t& out) noexcept {
        out = 0;
        for (unsigned i = 0; i < 5; ++i) {
            unsigned b{}; if (!byte(b) || (i == 4 && (b & 0xf0))) return false;
            out |= (b & 0x7fU) << (i * 7);
            if (!(b & 0x80)) return true;
        }
        return false;
    }
    bool take(std::uint32_t size, std::span<const std::byte>& out) noexcept {
        if (size > bytes_.size() - position_) return false;
        out = bytes_.subspan(position_, size); position_ += size; return true;
    }
    bool name(std::string_view& out) noexcept {
        std::uint32_t size{}; std::span<const std::byte> bytes;
        if (!u32(size) || !take(size, bytes) || !valid_utf8(bytes)) return false;
        if (std::find(bytes.begin(), bytes.end(), std::byte{0}) != bytes.end()) return false;
        out = {reinterpret_cast<const char*>(bytes.data()), bytes.size()}; return true;
    }
    bool empty() const noexcept { return position_ == bytes_.size(); }
  private:
    std::span<const std::byte> bytes_;
    std::size_t position_{};
};
}
bool passive_module(std::span<const std::byte> bytes, const CapabilityRegistry* capabilities) noexcept {
    constexpr std::array header{std::byte{0}, std::byte{0x61}, std::byte{0x73}, std::byte{0x6d},
        std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0}};
    if (bytes.size() < header.size() || bytes.size() > 16384 || !std::equal(header.begin(), header.end(), bytes.begin())) return false;
    Reader module(bytes.subspan(header.size()));
    while (!module.empty()) {
        unsigned id{}; std::uint32_t size{}; std::span<const std::byte> payload;
        if (!module.byte(id) || id == 8 || !module.u32(size) || !module.take(size, payload)) return false;
        if (id != 2 && id != 7) continue;
        Reader section(payload); std::uint32_t count{};
        if (!section.u32(count) || (id == 2 && count > kMaximumCapabilityFunctions)) return false;
        for (std::uint32_t i = 0; i < count; ++i) {
            std::string_view first, second; unsigned kind{}; std::uint32_t index{};
            if (!section.name(first)) return false;
            if (id == 2 && !section.name(second)) return false;
            if (!section.byte(kind) || !section.u32(index)) return false;
            if (id == 2) {
                if (kind != 0 || !capabilities || !capabilities->resolve(first, second)) return false;
            } else if (kind == 0 && (first == "__post_instantiate" || first == "__wasm_call_ctors" || first == "_initialize")) return false;
        }
        if (!section.empty()) return false;
    }
    return true;
}
}
