#include "blip/storage/imported_settings.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>

namespace blip::storage {
namespace {

constexpr std::uint32_t kEncodedMagic = 0x56494c42U; // "BLIV".
constexpr std::size_t kCrcOffset = 12;
constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

[[nodiscard]] core::Error codec_error(core::ErrorCode code, std::string_view operation,
                                      std::string_view detail) noexcept {
    return {core::ErrorDomain::storage, code, "blip.storage.legacy-import", operation, detail};
}

void write_u16(std::span<std::byte> output, std::size_t offset, std::uint16_t value) noexcept {
    output[offset] = static_cast<std::byte>(value & 0xffU);
    output[offset + 1U] = static_cast<std::byte>(value >> 8U);
}

void write_u32(std::span<std::byte> output, std::size_t offset, std::uint32_t value) noexcept {
    for (std::size_t index = 0; index < 4; ++index) {
        output[offset + index] = static_cast<std::byte>(value >> (index * 8U));
    }
}

void write_u64(std::span<std::byte> output, std::size_t offset, std::uint64_t value) noexcept {
    for (std::size_t index = 0; index < 8; ++index) {
        output[offset + index] = static_cast<std::byte>(value >> (index * 8U));
    }
}

[[nodiscard]] std::uint16_t read_u16(std::span<const std::byte> input,
                                     std::size_t offset) noexcept {
    return static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(input[offset])) |
           static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(input[offset + 1U])) << 8U;
}

[[nodiscard]] std::uint32_t read_u32(std::span<const std::byte> input,
                                     std::size_t offset) noexcept {
    std::uint32_t value{};
    for (std::size_t index = 0; index < 4; ++index) {
        value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(input[offset + index]))
                 << (index * 8U);
    }
    return value;
}

[[nodiscard]] std::uint64_t read_u64(std::span<const std::byte> input,
                                     std::size_t offset) noexcept {
    std::uint64_t value{};
    for (std::size_t index = 0; index < 8; ++index) {
        value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(input[offset + index]))
                 << (index * 8U);
    }
    return value;
}

[[nodiscard]] std::uint32_t crc32(std::span<const std::byte> input) noexcept {
    std::uint32_t crc = 0xffffffffU;
    for (std::size_t index = 0; index < input.size(); ++index) {
        const std::uint8_t byte = index >= kCrcOffset && index < kCrcOffset + 4U
                                      ? 0U
                                      : std::to_integer<std::uint8_t>(input[index]);
        crc ^= byte;
        for (std::uint8_t bit = 0; bit < 8; ++bit) {
            const std::uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0xedb88320U & mask);
        }
    }
    return ~crc;
}

[[nodiscard]] std::uint64_t field_hash(std::string_view field) noexcept {
    std::uint64_t hash = kFnvOffset;
    for (const char character : field) {
        hash ^= static_cast<std::uint8_t>(character);
        hash *= kFnvPrime;
    }
    return hash;
}

[[nodiscard]] bool valid_field(std::string_view field) noexcept {
    if (field.empty() || field.size() > 48U) {
        return false;
    }
    for (const char character : field) {
        if (!((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
              (character >= '0' && character <= '9') || character == '_' || character == '-')) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool valid_utf8(std::string_view value) noexcept {
    std::size_t index{};
    while (index < value.size()) {
        const auto first = static_cast<std::uint8_t>(value[index]);
        if (first <= 0x7fU) {
            ++index;
            continue;
        }
        std::size_t trailing{};
        std::uint32_t code_point{};
        if (first >= 0xc2U && first <= 0xdfU) {
            trailing = 1;
            code_point = first & 0x1fU;
        } else if (first >= 0xe0U && first <= 0xefU) {
            trailing = 2;
            code_point = first & 0x0fU;
        } else if (first >= 0xf0U && first <= 0xf4U) {
            trailing = 3;
            code_point = first & 0x07U;
        } else {
            return false;
        }
        if (index + trailing >= value.size()) {
            return false;
        }
        for (std::size_t offset = 1; offset <= trailing; ++offset) {
            const auto next = static_cast<std::uint8_t>(value[index + offset]);
            if ((next & 0xc0U) != 0x80U) {
                return false;
            }
            code_point = (code_point << 6U) | (next & 0x3fU);
        }
        if ((trailing == 2 && code_point < 0x800U) || (trailing == 3 && code_point < 0x10000U) ||
            code_point > 0x10ffffU || (code_point >= 0xd800U && code_point <= 0xdfffU)) {
            return false;
        }
        index += trailing + 1U;
    }
    return true;
}

} // namespace

void ImportedSettingsBuilder::reset() noexcept {
    size_ = kImportedSettingsHeaderBytes;
    field_count_ = 0;
    finished_ = false;
    if (output_.size() >= kImportedSettingsHeaderBytes) {
        std::fill_n(output_.data(), kImportedSettingsHeaderBytes, std::byte{0});
    }
}

core::Status ImportedSettingsBuilder::append(const ImportedSetting& setting) noexcept {
    if (finished_ || !valid_field(setting.field) ||
        field_count_ == kMaxImportedFieldsPerComponent) {
        return core::Status::failure(codec_error(field_count_ == kMaxImportedFieldsPerComponent
                                                     ? core::ErrorCode::capacity_exceeded
                                                     : core::ErrorCode::invalid_argument,
                                                 "encode-import", "invalid-field-or-state"));
    }
    std::size_t value_size{};
    switch (setting.value.type) {
    case LegacyValueType::boolean:
        value_size = 1;
        break;
    case LegacyValueType::signed_integer:
    case LegacyValueType::unsigned_integer:
    case LegacyValueType::floating:
        value_size = 8;
        break;
    case LegacyValueType::string:
        value_size = setting.value.string.size();
        break;
    case LegacyValueType::numeric_array:
        if (setting.value.numeric_array.size() > kMaxLegacyNumericArrayValues) {
            return core::Status::failure(
                codec_error(core::ErrorCode::capacity_exceeded, "encode-import", "array-too-long"));
        }
        value_size = 1U + setting.value.numeric_array.size() * 8U;
        break;
    }
    if (value_size > std::numeric_limits<std::uint16_t>::max() ||
        size_ + 4U + setting.field.size() + value_size > output_.size()) {
        return core::Status::failure(
            codec_error(core::ErrorCode::capacity_exceeded, "encode-import", "payload-full"));
    }
    output_[size_] = static_cast<std::byte>(setting.value.type);
    output_[size_ + 1U] = static_cast<std::byte>(setting.field.size());
    write_u16(output_, size_ + 2U, static_cast<std::uint16_t>(value_size));
    size_ += 4U;
    std::memcpy(output_.data() + size_, setting.field.data(), setting.field.size());
    size_ += setting.field.size();
    switch (setting.value.type) {
    case LegacyValueType::boolean:
        output_[size_] = setting.value.boolean ? std::byte{1} : std::byte{0};
        break;
    case LegacyValueType::signed_integer:
        write_u64(output_, size_, std::bit_cast<std::uint64_t>(setting.value.signed_integer));
        break;
    case LegacyValueType::unsigned_integer:
        write_u64(output_, size_, setting.value.unsigned_integer);
        break;
    case LegacyValueType::floating:
        write_u64(output_, size_, std::bit_cast<std::uint64_t>(setting.value.floating));
        break;
    case LegacyValueType::string:
        if (!setting.value.string.empty()) {
            std::memcpy(output_.data() + size_, setting.value.string.data(),
                        setting.value.string.size());
        }
        break;
    case LegacyValueType::numeric_array:
        output_[size_] = static_cast<std::byte>(setting.value.numeric_array.size());
        for (std::size_t index = 0; index < setting.value.numeric_array.size(); ++index) {
            write_u64(output_, size_ + 1U + index * 8U,
                      std::bit_cast<std::uint64_t>(setting.value.numeric_array[index]));
        }
        break;
    }
    size_ += value_size;
    ++field_count_;
    return core::Status::success();
}

core::Result<std::size_t> ImportedSettingsBuilder::finish() noexcept {
    if (finished_ || output_.size() < kImportedSettingsHeaderBytes ||
        field_count_ > std::numeric_limits<std::uint16_t>::max()) {
        return core::Result<std::size_t>::failure(
            codec_error(core::ErrorCode::invalid_state, "encode-import", "invalid-builder-state"));
    }
    write_u32(output_, 0, kEncodedMagic);
    write_u16(output_, 4, kImportedSettingsFormatVersion);
    write_u16(output_, 6, static_cast<std::uint16_t>(kImportedSettingsHeaderBytes));
    write_u16(output_, 8, static_cast<std::uint16_t>(field_count_));
    write_u32(output_, kCrcOffset, crc32(std::span<const std::byte>{output_.data(), size_}));
    finished_ = true;
    return core::Result<std::size_t>::success(size_);
}

core::Result<std::size_t> ImportedSettingsView::visit(std::span<const std::byte> payload,
                                                      ImportedSettingsDecodeWorkspace& workspace,
                                                      ImportedSettingVisitor visitor,
                                                      void* context) const noexcept {
    workspace.field_hashes.fill(0);
    if (payload.size() < kImportedSettingsHeaderBytes || read_u32(payload, 0) != kEncodedMagic ||
        read_u16(payload, 4) != kImportedSettingsFormatVersion ||
        read_u16(payload, 6) != kImportedSettingsHeaderBytes || read_u16(payload, 10) != 0U ||
        read_u32(payload, kCrcOffset) != crc32(payload)) {
        return core::Result<std::size_t>::failure(
            codec_error(core::ErrorCode::corrupt_data, "decode-import", "invalid-header-or-crc"));
    }
    const std::size_t field_count = read_u16(payload, 8);
    if (field_count > kMaxImportedFieldsPerComponent ||
        (visitor == nullptr && context != nullptr)) {
        return core::Result<std::size_t>::failure(
            codec_error(core::ErrorCode::capacity_exceeded, "decode-import", "too-many-fields"));
    }
    std::size_t offset = kImportedSettingsHeaderBytes;
    for (std::size_t index = 0; index < field_count; ++index) {
        if (payload.size() - offset < 4U) {
            return core::Result<std::size_t>::failure(
                codec_error(core::ErrorCode::corrupt_data, "decode-import", "truncated-entry"));
        }
        const auto type =
            static_cast<LegacyValueType>(std::to_integer<std::uint8_t>(payload[offset]));
        const std::size_t field_size = std::to_integer<std::uint8_t>(payload[offset + 1U]);
        const std::size_t value_size = read_u16(payload, offset + 2U);
        offset += 4U;
        if (field_size > payload.size() - offset ||
            value_size > payload.size() - offset - field_size) {
            return core::Result<std::size_t>::failure(
                codec_error(core::ErrorCode::corrupt_data, "decode-import", "invalid-length"));
        }
        const std::string_view field{reinterpret_cast<const char*>(payload.data() + offset),
                                     field_size};
        offset += field_size;
        if (!valid_field(field)) {
            return core::Result<std::size_t>::failure(
                codec_error(core::ErrorCode::corrupt_data, "decode-import", "invalid-field"));
        }
        const std::uint64_t hash = field_hash(field);
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (workspace.field_hashes[prior] == hash) {
                return core::Result<std::size_t>::failure(
                    codec_error(core::ErrorCode::duplicate_id, "decode-import", "duplicate-field"));
            }
        }
        workspace.field_hashes[index] = hash;
        const auto value_bytes = payload.subspan(offset, value_size);
        LegacyValue value{};
        value.type = type;
        switch (type) {
        case LegacyValueType::boolean:
            if (value_size != 1U || std::to_integer<std::uint8_t>(value_bytes[0]) > 1U) {
                return core::Result<std::size_t>::failure(
                    codec_error(core::ErrorCode::corrupt_data, "decode-import", "invalid-boolean"));
            }
            value.boolean = value_bytes[0] == std::byte{1};
            break;
        case LegacyValueType::signed_integer:
            if (value_size != 8U) {
                return core::Result<std::size_t>::failure(
                    codec_error(core::ErrorCode::corrupt_data, "decode-import", "invalid-integer"));
            }
            value.signed_integer = std::bit_cast<std::int64_t>(read_u64(value_bytes, 0));
            break;
        case LegacyValueType::unsigned_integer:
            if (value_size != 8U) {
                return core::Result<std::size_t>::failure(
                    codec_error(core::ErrorCode::corrupt_data, "decode-import", "invalid-integer"));
            }
            value.unsigned_integer = read_u64(value_bytes, 0);
            break;
        case LegacyValueType::floating:
            if (value_size != 8U) {
                return core::Result<std::size_t>::failure(
                    codec_error(core::ErrorCode::corrupt_data, "decode-import", "invalid-float"));
            }
            value.floating = std::bit_cast<double>(read_u64(value_bytes, 0));
            if (!std::isfinite(value.floating)) {
                return core::Result<std::size_t>::failure(
                    codec_error(core::ErrorCode::corrupt_data, "decode-import", "non-finite"));
            }
            break;
        case LegacyValueType::string:
            value.string = {reinterpret_cast<const char*>(value_bytes.data()), value_bytes.size()};
            if (!valid_utf8(value.string)) {
                return core::Result<std::size_t>::failure(
                    codec_error(core::ErrorCode::corrupt_data, "decode-import", "invalid-utf8"));
            }
            break;
        case LegacyValueType::numeric_array: {
            if (value_size == 0U) {
                return core::Result<std::size_t>::failure(
                    codec_error(core::ErrorCode::corrupt_data, "decode-import", "invalid-array"));
            }
            const std::size_t count = std::to_integer<std::uint8_t>(value_bytes[0]);
            if (count > kMaxLegacyNumericArrayValues || value_size != 1U + count * 8U) {
                return core::Result<std::size_t>::failure(
                    codec_error(core::ErrorCode::corrupt_data, "decode-import", "invalid-array"));
            }
            for (std::size_t item = 0; item < count; ++item) {
                workspace.numeric_array[item] =
                    std::bit_cast<double>(read_u64(value_bytes, 1U + item * 8U));
                if (!std::isfinite(workspace.numeric_array[item])) {
                    return core::Result<std::size_t>::failure(codec_error(
                        core::ErrorCode::corrupt_data, "decode-import", "non-finite-array"));
                }
            }
            value.numeric_array = {workspace.numeric_array.data(), count};
            break;
        }
        default:
            return core::Result<std::size_t>::failure(codec_error(
                core::ErrorCode::incompatible_version, "decode-import", "unknown-value-type"));
        }
        if (visitor != nullptr) {
            const ImportedSetting setting{{}, {}, field, value};
            const auto status = visitor(context, setting);
            if (!status) {
                return core::Result<std::size_t>::failure(status.error());
            }
        }
        offset += value_size;
    }
    if (offset != payload.size()) {
        return core::Result<std::size_t>::failure(
            codec_error(core::ErrorCode::corrupt_data, "decode-import", "trailing-data"));
    }
    return core::Result<std::size_t>::success(field_count);
}

} // namespace blip::storage
