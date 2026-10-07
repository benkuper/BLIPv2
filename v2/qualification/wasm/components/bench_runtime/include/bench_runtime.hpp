#pragma once
#include <cstddef>
#include <cstdint>

namespace bench {
inline constexpr std::size_t pool_bytes = 128 * 1024;
inline constexpr std::uint32_t wasm_stack_bytes = 8192;
bool load(std::uint8_t* bytes, std::size_t length, char* error, std::size_t capacity);
bool call(const char* name, std::uint32_t argument, std::uint32_t& result, char* error, std::size_t capacity, int fuel = 2000000);
void close();
std::uint32_t reserved_bytes();
std::uint32_t used_bytes();
std::uint32_t peak_bytes();
std::uint32_t probe_calls();
bool initial_memory_is_zero();
std::uint32_t integer_work(std::uint32_t count);
std::uint32_t pixel_work(std::uint32_t count);
}
