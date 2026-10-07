#include "blip/wasm/utf8.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <initializer_list>
#include <iostream>

namespace {
using namespace blip::wasm;
using blip::core::ErrorCode;
#define CHECK(x) do { if (!(x)) { std::cerr << "FAIL " << __func__ << ':' << __LINE__ << " " #x "\n"; return false; } } while (false)
struct Memory final : GuestMemory {
    std::array<std::byte, 512> bytes{};
    unsigned reads{}, writes{};
    bool fail{};
    blip::core::Status copy(std::uint32_t offset, std::size_t count) const {
        if (fail || offset > bytes.size() || count > bytes.size() - offset)
            return blip::core::Status::failure({blip::core::ErrorDomain::control, ErrorCode::invalid_argument,
                                               "test.memory", "copy", "bounds"});
        return blip::core::Status::success();
    }
    blip::core::Status read_memory(std::uint32_t offset, std::span<std::byte> output) noexcept override {
        ++reads;
        const auto status = copy(offset, output.size());
        if (status && !output.empty()) std::memmove(output.data(), bytes.data() + offset, output.size());
        return status;
    }
    blip::core::Status write_memory(std::uint32_t offset, std::span<const std::byte> input) noexcept override {
        ++writes;
        const auto status = copy(offset, input.size());
        if (status && !input.empty()) std::memmove(bytes.data() + offset, input.data(), input.size());
        return status;
    }
};
bool encoding(std::initializer_list<unsigned> input) {
    std::array<std::byte, 12> bytes{};
    std::size_t count{};
    for (auto value : input) bytes[count++] = static_cast<std::byte>(value);
    return valid_utf8(std::span(bytes).first(count));
}
bool unicode_boundaries() {
    CHECK(encoding({})); CHECK(encoding({0})); CHECK(encoding({0x7f}));
    for (auto input : {std::initializer_list<unsigned>{0xc2,0x80}, {0xdf,0xbf}, {0xe0,0xa0,0x80},
                      {0xed,0x9f,0xbf}, {0xee,0x80,0x80}, {0xef,0xbf,0xbf}, {0xf0,0x90,0x80,0x80}, {0xf4,0x8f,0xbf,0xbf}})
        CHECK(encoding(input));
    for (auto input : {std::initializer_list<unsigned>{0x80}, {0xbf}, {0xc0,0x80}, {0xc1,0xbf}, {0xc2},
                      {0xc2,0x7f}, {0xe0,0x9f,0xbf}, {0xed,0xa0,0x80}, {0xed,0xbf,0xbf},
                      {0xe1,0x80}, {0xe1,0x80,0x7f}, {0xf0,0x8f,0xbf,0xbf}, {0xf4,0x90,0x80,0x80},
                      {0xf5,0x80,0x80,0x80}, {0xff}, {0xf0,0x90,0x80}, {0xf0,0x90,0x80,0x7f}})
        CHECK(!encoding(input));
    // Every valid Unicode scalar is encoded independently and accepted.
    // This checks all continuation/boundary classes without relying on locale.
    for (unsigned scalar = 0; scalar <= 0x10ffff; ++scalar) {
        if (scalar >= 0xd800 && scalar <= 0xdfff) continue;
        if (scalar < 0x80) CHECK(encoding({scalar}));
        else if (scalar < 0x800) CHECK(encoding({0xc0 | (scalar >> 6), 0x80 | (scalar & 63)}));
        else if (scalar < 0x10000) CHECK(encoding({0xe0 | (scalar >> 12), 0x80 | ((scalar >> 6) & 63), 0x80 | (scalar & 63)}));
        else CHECK(encoding({0xf0 | (scalar >> 18), 0x80 | ((scalar >> 12) & 63), 0x80 | ((scalar >> 6) & 63), 0x80 | (scalar & 63)}));
    }
    return true;
}
bool exact_bytes_and_no_terminator() {
    Memory memory;
    constexpr std::array text{'A','\0','Z'};
    CHECK(write_utf8(memory, 10, {text.data(), text.size()}));
    std::array<char, 4> output{'x','x','x','x'};
    std::size_t count = 99;
    CHECK(read_utf8(memory, {10,3}, output, count));
    CHECK(count == 3 && std::equal(text.begin(), text.end(), output.begin()) && output[3] == 'x');
    memory.bytes.back() = std::byte{'z'};
    CHECK(read_utf8(memory, {511,1}, output, count));
    CHECK(count == 1 && output[0] == 'z');
    CHECK(read_utf8(memory, {512,0}, {}, count)); CHECK(count == 0);
    CHECK(write_utf8(memory, 512, {}));
    CHECK(!read_utf8(memory, {513,0}, output, count)); CHECK(count == 0);
    CHECK(!write_utf8(memory, 513, {}));
    return true;
}
bool limit_and_failure_atomicity() {
    Memory memory;
    std::array<char, kMaximumStringBytes> text{}; text.fill('Q');
    CHECK(write_utf8(memory, 256, {text.data(), text.size()}));
    std::array<char, kMaximumStringBytes + 1> output{}; output.fill('x');
    std::size_t count{};
    CHECK(read_utf8(memory, {256,256}, output, count)); CHECK(count == 256 && output[256] == 'x');
    std::array<char, kMaximumStringBytes + 1> long_text{}; long_text.fill('Q');
    const auto before = memory.bytes;
    const auto writes = memory.writes, reads = memory.reads;
    CHECK(!write_utf8(memory, 0, {long_text.data(),long_text.size()})); CHECK(memory.writes == writes);
    output.fill('x');
    CHECK(!read_utf8(memory, {0,257}, output, count)); CHECK(count == 0 && memory.reads == reads);
    CHECK(!read_utf8(memory, {0,256}, std::span(output).first(255), count)); CHECK(memory.reads == reads);
    for (const StringRef span : {StringRef{UINT32_MAX,1}, {UINT32_MAX,0}, {1,UINT32_MAX}, {500,13}, {513,1}}) {
        CHECK(!read_utf8(memory, span, output, count)); CHECK(count == 0);
        CHECK(std::all_of(output.begin(), output.end(), [](char c) { return c == 'x'; }));
    }
    CHECK(!write_utf8(memory, UINT32_MAX, "x")); CHECK(!write_utf8(memory, 511, "xy"));
    CHECK(memory.bytes == before);
    constexpr std::array invalid{'\xe0','\x80','\x80'};
    CHECK(!write_utf8(memory, 0, {invalid.data(),invalid.size()})); CHECK(memory.bytes == before);
    std::memcpy(memory.bytes.data(), invalid.data(), invalid.size());
    CHECK(!read_utf8(memory, {0,3}, output, count)); CHECK(count == 0);
    CHECK(std::all_of(output.begin(), output.end(), [](char c) { return c == 'x'; }));
    memory.fail = true;
    CHECK(!read_utf8(memory, {0,1}, output, count)); CHECK(count == 0);
    return true;
}
}
int main() {
    if (!unicode_boundaries() || !exact_bytes_and_no_terminator() || !limit_and_failure_atomicity()) return 1;
    std::cout << "Checked guest UTF-8: scalar, malformed, bounds, copy and failure-atomicity cases passed\n";
}
