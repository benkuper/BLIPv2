#include "blip/wasm/module_upload.hpp"
#include <cstring>

namespace blip::wasm {
namespace {
core::Status failure(core::ErrorCode code, std::string_view detail) noexcept {
    return core::Status::failure({core::ErrorDomain::control, code, "blip.wasm", "upload", detail});
}
}
core::Status ModuleUpload::begin(std::size_t bytes, std::uint32_t crc) noexcept {
    if (bytes < 8 || bytes > storage_.size()) return failure(core::ErrorCode::capacity_exceeded, "module-size");
    expected_ = bytes;
    received_ = 0;
    expected_crc_ = crc;
    crc_ = 0xffffffffU;
    return core::Status::success();
}
core::Status ModuleUpload::append(std::size_t offset, std::span<const std::byte> bytes) noexcept {
    if (!expected_) return failure(core::ErrorCode::invalid_state, "upload-not-started");
    if (offset != received_ || bytes.empty() || bytes.size() > expected_ - received_)
        return failure(core::ErrorCode::invalid_argument, "upload-offset-or-size");
    for (const auto value : bytes) {
        crc_ ^= std::to_integer<std::uint8_t>(value);
        for (unsigned bit = 0; bit < 8; ++bit) crc_ = (crc_ >> 1) ^ (0xedb88320U & (0U - (crc_ & 1U)));
    }
    std::memmove(storage_.data() + offset, bytes.data(), bytes.size());
    received_ += bytes.size();
    return core::Status::success();
}
core::Result<std::span<const std::byte>> ModuleUpload::finish() noexcept {
    using Result = core::Result<std::span<const std::byte>>;
    if (!expected_ || expected_ != received_)
        return Result::failure(failure(core::ErrorCode::invalid_state, "upload-incomplete").error());
    const auto size = expected_;
    expected_ = 0;
    if (~crc_ != expected_crc_)
        return Result::failure(failure(core::ErrorCode::verification_failed, "module-crc").error());
    return Result::success(storage_.first(size));
}
} // namespace blip::wasm
