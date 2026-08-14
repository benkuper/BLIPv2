#include "blip/storage/legacy_settings_importer.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>

namespace blip::storage {
namespace {

constexpr std::array<LegacyComponentMapping, 22> kMappings{{
    {"comm", "blip.transport"},
    {"comm.serial", "blip.transport.serial"},
    {"comm.osc", "blip.osc"},
    {"comm.espnow", "blip.transport.espnow"},
    {"comm.server", "blip.oscquery"},
    {"settings", "blip.system.settings"},
    {"leds", "blip.led"},
    {"leds.strip1", "blip.led.strip.1"},
    {"leds.strip1.playbackLayer", "blip.led.strip.1.layer.playback"},
    {"leds.strip1.streamLayer", "blip.led.strip.1.layer.stream"},
    {"leds.strip1.scriptLayer", "blip.led.strip.1.layer.script"},
    {"leds.strip1.systemLayer", "blip.led.strip.1.layer.system"},
    {"leds.strip1.fx", "blip.led.strip.1.fx"},
    {"wifi", "blip.transport.wifi"},
    {"battery", "blip.power.battery"},
    {"files", "blip.storage.files"},
    {"script", "blip.wasm"},
    {"dmxReceiver", "blip.transport.dmx.receiver"},
    {"buttons", "blip.input.buttons"},
    {"buttons.button1", "blip.input.button.1"},
    {"ir", "blip.input.ir"},
    {"motion", "blip.sensor.motion"},
}};

constexpr std::size_t kMaxDepth = 8;
constexpr std::size_t kMaxMapEntries = 64;
constexpr std::size_t kMaxNameBytes = 48;
constexpr std::size_t kMaxStringBytes = 256;
constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

[[nodiscard]] core::Error import_error(core::ErrorCode code, std::string_view operation,
                                       std::string_view detail) noexcept {
    return {core::ErrorDomain::storage, code, "blip.storage.legacy-import", operation, detail};
}

[[nodiscard]] std::uint32_t rotate_right(std::uint32_t value, std::uint32_t count) noexcept {
    return (value >> count) | (value << (32U - count));
}

class Sha256 final {
  public:
    void update(std::span<const std::byte> input) noexcept {
        for (const std::byte value : input) {
            buffer_[buffer_size_++] = std::to_integer<std::uint8_t>(value);
            ++total_size_;
            if (buffer_size_ == buffer_.size()) {
                transform(buffer_);
                buffer_size_ = 0;
            }
        }
    }

    [[nodiscard]] std::array<std::byte, 32> finish() noexcept {
        const std::uint64_t bit_size = total_size_ * 8U;
        buffer_[buffer_size_++] = 0x80U;
        if (buffer_size_ > 56U) {
            std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(buffer_size_), buffer_.end(),
                      std::uint8_t{0});
            transform(buffer_);
            buffer_size_ = 0;
        }
        std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(buffer_size_), buffer_.begin() + 56,
                  std::uint8_t{0});
        for (std::size_t index = 0; index < 8; ++index) {
            buffer_[63U - index] = static_cast<std::uint8_t>(bit_size >> (index * 8U));
        }
        transform(buffer_);
        std::array<std::byte, 32> output{};
        for (std::size_t word = 0; word < state_.size(); ++word) {
            for (std::size_t byte = 0; byte < 4; ++byte) {
                output[word * 4U + byte] =
                    static_cast<std::byte>(state_[word] >> ((3U - byte) * 8U));
            }
        }
        return output;
    }

  private:
    void transform(const std::array<std::uint8_t, 64>& block) noexcept {
        static constexpr std::array<std::uint32_t, 64> constants{
            0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U,
            0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
            0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U,
            0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
            0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
            0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
            0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
            0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
            0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU,
            0x5b9cca4fU, 0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
            0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
        };
        std::array<std::uint32_t, 64> words{};
        for (std::size_t index = 0; index < 16; ++index) {
            words[index] = static_cast<std::uint32_t>(block[index * 4U]) << 24U |
                           static_cast<std::uint32_t>(block[index * 4U + 1U]) << 16U |
                           static_cast<std::uint32_t>(block[index * 4U + 2U]) << 8U |
                           static_cast<std::uint32_t>(block[index * 4U + 3U]);
        }
        for (std::size_t index = 16; index < words.size(); ++index) {
            const std::uint32_t s0 = rotate_right(words[index - 15U], 7U) ^
                                     rotate_right(words[index - 15U], 18U) ^
                                     (words[index - 15U] >> 3U);
            const std::uint32_t s1 = rotate_right(words[index - 2U], 17U) ^
                                     rotate_right(words[index - 2U], 19U) ^
                                     (words[index - 2U] >> 10U);
            words[index] = words[index - 16U] + s0 + words[index - 7U] + s1;
        }
        std::uint32_t a = state_[0];
        std::uint32_t b = state_[1];
        std::uint32_t c = state_[2];
        std::uint32_t d = state_[3];
        std::uint32_t e = state_[4];
        std::uint32_t f = state_[5];
        std::uint32_t g = state_[6];
        std::uint32_t h = state_[7];
        for (std::size_t index = 0; index < words.size(); ++index) {
            const std::uint32_t sum1 =
                rotate_right(e, 6U) ^ rotate_right(e, 11U) ^ rotate_right(e, 25U);
            const std::uint32_t choose = (e & f) ^ (~e & g);
            const std::uint32_t temp1 = h + sum1 + choose + constants[index] + words[index];
            const std::uint32_t sum0 =
                rotate_right(a, 2U) ^ rotate_right(a, 13U) ^ rotate_right(a, 22U);
            const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temp2 = sum0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }
        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    std::array<std::uint32_t, 8> state_{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                                        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t buffer_size_{};
    std::uint64_t total_size_{};
};

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

[[nodiscard]] bool valid_name(std::string_view value) noexcept {
    if (value.empty() || value.size() > kMaxNameBytes) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](char character) {
        return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') || character == '_' || character == '-';
    });
}

[[nodiscard]] std::uint64_t hash_text(std::string_view first,
                                      std::string_view second = {}) noexcept {
    std::uint64_t hash = kFnvOffset;
    for (const char character : first) {
        hash ^= static_cast<std::uint8_t>(character);
        hash *= kFnvPrime;
    }
    hash ^= 0U;
    hash *= kFnvPrime;
    for (const char character : second) {
        hash ^= static_cast<std::uint8_t>(character);
        hash *= kFnvPrime;
    }
    return hash;
}

class Parser final {
  public:
    Parser(std::span<const std::byte> source, LegacyImportWorkspace& workspace,
           ImportedSettingVisitor visitor, void* context) noexcept
        : source_(source), workspace_(&workspace), visitor_(visitor), context_(context) {}

    [[nodiscard]] core::Result<LegacyImportSummary> parse() noexcept {
        workspace_->setting_hashes.fill(0);
        workspace_->component_hashes.fill(0);
        workspace_->component_path.fill('\0');
        const auto root_count = read_map_count();
        if (!root_count) {
            return core::Result<LegacyImportSummary>::failure(root_count.error());
        }
        bool found_components{};
        for (std::size_t index = 0; index < root_count.value(); ++index) {
            const auto key = read_string(kMaxNameBytes);
            if (!key) {
                return core::Result<LegacyImportSummary>::failure(key.error());
            }
            if (key.value() != "components" || found_components) {
                return core::Result<LegacyImportSummary>::failure(import_error(
                    core::ErrorCode::incompatible_version, "parse-root", "unexpected-root-key"));
            }
            found_components = true;
            const auto status = parse_component_collection(1);
            if (!status) {
                return core::Result<LegacyImportSummary>::failure(status.error());
            }
        }
        if (!found_components || offset_ != source_.size()) {
            return core::Result<LegacyImportSummary>::failure(
                import_error(core::ErrorCode::corrupt_data, "parse-root",
                             "missing-components-or-trailing-data"));
        }
        Sha256 sha{};
        sha.update(source_);
        return core::Result<LegacyImportSummary>::success(
            {sha.finish(), setting_count_, component_count_});
    }

  private:
    [[nodiscard]] core::Result<std::uint8_t> read_byte() noexcept {
        if (offset_ == source_.size()) {
            return core::Result<std::uint8_t>::failure(
                import_error(core::ErrorCode::corrupt_data, "decode", "truncated"));
        }
        return core::Result<std::uint8_t>::success(
            std::to_integer<std::uint8_t>(source_[offset_++]));
    }

    template <typename T> [[nodiscard]] core::Result<T> read_big_endian() noexcept {
        if (source_.size() - offset_ < sizeof(T)) {
            return core::Result<T>::failure(
                import_error(core::ErrorCode::corrupt_data, "decode", "truncated-number"));
        }
        T value{};
        for (std::size_t index = 0; index < sizeof(T); ++index) {
            value = static_cast<T>((value << 8U) |
                                   std::to_integer<std::uint8_t>(source_[offset_ + index]));
        }
        offset_ += sizeof(T);
        return core::Result<T>::success(value);
    }

    [[nodiscard]] core::Result<std::size_t> read_map_count() noexcept {
        const auto marker = read_byte();
        if (!marker) {
            return core::Result<std::size_t>::failure(marker.error());
        }
        std::uint32_t count{};
        if ((marker.value() & 0xf0U) == 0x80U) {
            count = marker.value() & 0x0fU;
        } else if (marker.value() == 0xdeU) {
            const auto value = read_big_endian<std::uint16_t>();
            if (!value) {
                return core::Result<std::size_t>::failure(value.error());
            }
            count = value.value();
        } else if (marker.value() == 0xdfU) {
            const auto value = read_big_endian<std::uint32_t>();
            if (!value) {
                return core::Result<std::size_t>::failure(value.error());
            }
            count = value.value();
        } else {
            return core::Result<std::size_t>::failure(
                import_error(core::ErrorCode::corrupt_data, "decode-map", "map-required"));
        }
        if (count > kMaxMapEntries) {
            return core::Result<std::size_t>::failure(import_error(
                core::ErrorCode::capacity_exceeded, "decode-map", "too-many-map-entries"));
        }
        return core::Result<std::size_t>::success(count);
    }

    [[nodiscard]] core::Result<std::size_t> read_array_count(std::uint8_t marker) noexcept {
        std::uint32_t count{};
        if ((marker & 0xf0U) == 0x90U) {
            count = marker & 0x0fU;
        } else if (marker == 0xdcU) {
            const auto value = read_big_endian<std::uint16_t>();
            if (!value) {
                return core::Result<std::size_t>::failure(value.error());
            }
            count = value.value();
        } else if (marker == 0xddU) {
            const auto value = read_big_endian<std::uint32_t>();
            if (!value) {
                return core::Result<std::size_t>::failure(value.error());
            }
            count = value.value();
        } else {
            return core::Result<std::size_t>::failure(
                import_error(core::ErrorCode::corrupt_data, "decode-array", "array-required"));
        }
        if (count > kMaxLegacyNumericArrayValues) {
            return core::Result<std::size_t>::failure(
                import_error(core::ErrorCode::capacity_exceeded, "decode-array", "array-too-long"));
        }
        return core::Result<std::size_t>::success(count);
    }

    [[nodiscard]] core::Result<std::string_view> read_string(std::size_t maximum) noexcept {
        const auto marker = read_byte();
        if (!marker) {
            return core::Result<std::string_view>::failure(marker.error());
        }
        std::uint32_t length{};
        if ((marker.value() & 0xe0U) == 0xa0U) {
            length = marker.value() & 0x1fU;
        } else if (marker.value() == 0xd9U) {
            const auto value = read_byte();
            if (!value) {
                return core::Result<std::string_view>::failure(value.error());
            }
            length = value.value();
        } else if (marker.value() == 0xdaU) {
            const auto value = read_big_endian<std::uint16_t>();
            if (!value) {
                return core::Result<std::string_view>::failure(value.error());
            }
            length = value.value();
        } else {
            return core::Result<std::string_view>::failure(
                import_error(core::ErrorCode::corrupt_data, "decode-string", "string-required"));
        }
        if (length > maximum || length > source_.size() - offset_) {
            return core::Result<std::string_view>::failure(
                import_error(length > maximum ? core::ErrorCode::capacity_exceeded
                                              : core::ErrorCode::corrupt_data,
                             "decode-string", "invalid-string-length"));
        }
        const std::string_view result{reinterpret_cast<const char*>(source_.data() + offset_),
                                      length};
        offset_ += length;
        if (!valid_utf8(result)) {
            return core::Result<std::string_view>::failure(
                import_error(core::ErrorCode::corrupt_data, "decode-string", "invalid-utf8"));
        }
        return core::Result<std::string_view>::success(result);
    }

    [[nodiscard]] core::Result<LegacyValue> read_number(std::uint8_t marker) noexcept {
        LegacyValue result{};
        if (marker <= 0x7fU) {
            result.type = LegacyValueType::unsigned_integer;
            result.unsigned_integer = marker;
            return core::Result<LegacyValue>::success(result);
        }
        if (marker >= 0xe0U) {
            result.type = LegacyValueType::signed_integer;
            result.signed_integer = std::bit_cast<std::int8_t>(marker);
            return core::Result<LegacyValue>::success(result);
        }
        if (marker >= 0xccU && marker <= 0xcfU) {
            result.type = LegacyValueType::unsigned_integer;
            if (marker == 0xccU) {
                const auto value = read_byte();
                if (!value) {
                    return core::Result<LegacyValue>::failure(value.error());
                }
                result.unsigned_integer = value.value();
            } else if (marker == 0xcdU) {
                const auto value = read_big_endian<std::uint16_t>();
                if (!value) {
                    return core::Result<LegacyValue>::failure(value.error());
                }
                result.unsigned_integer = value.value();
            } else if (marker == 0xceU) {
                const auto value = read_big_endian<std::uint32_t>();
                if (!value) {
                    return core::Result<LegacyValue>::failure(value.error());
                }
                result.unsigned_integer = value.value();
            } else {
                const auto value = read_big_endian<std::uint64_t>();
                if (!value) {
                    return core::Result<LegacyValue>::failure(value.error());
                }
                result.unsigned_integer = value.value();
            }
            return core::Result<LegacyValue>::success(result);
        }
        if (marker >= 0xd0U && marker <= 0xd3U) {
            result.type = LegacyValueType::signed_integer;
            if (marker == 0xd0U) {
                const auto value = read_byte();
                if (!value) {
                    return core::Result<LegacyValue>::failure(value.error());
                }
                result.signed_integer = std::bit_cast<std::int8_t>(value.value());
            } else if (marker == 0xd1U) {
                const auto value = read_big_endian<std::uint16_t>();
                if (!value) {
                    return core::Result<LegacyValue>::failure(value.error());
                }
                result.signed_integer = std::bit_cast<std::int16_t>(value.value());
            } else if (marker == 0xd2U) {
                const auto value = read_big_endian<std::uint32_t>();
                if (!value) {
                    return core::Result<LegacyValue>::failure(value.error());
                }
                result.signed_integer = std::bit_cast<std::int32_t>(value.value());
            } else {
                const auto value = read_big_endian<std::uint64_t>();
                if (!value) {
                    return core::Result<LegacyValue>::failure(value.error());
                }
                result.signed_integer = std::bit_cast<std::int64_t>(value.value());
            }
            return core::Result<LegacyValue>::success(result);
        }
        if (marker == 0xcaU || marker == 0xcbU) {
            result.type = LegacyValueType::floating;
            if (marker == 0xcaU) {
                const auto bits = read_big_endian<std::uint32_t>();
                if (!bits) {
                    return core::Result<LegacyValue>::failure(bits.error());
                }
                result.floating = std::bit_cast<float>(bits.value());
            } else {
                const auto bits = read_big_endian<std::uint64_t>();
                if (!bits) {
                    return core::Result<LegacyValue>::failure(bits.error());
                }
                result.floating = std::bit_cast<double>(bits.value());
            }
            if (!std::isfinite(result.floating)) {
                return core::Result<LegacyValue>::failure(
                    import_error(core::ErrorCode::corrupt_data, "decode-number", "non-finite"));
            }
            return core::Result<LegacyValue>::success(result);
        }
        return core::Result<LegacyValue>::failure(import_error(
            core::ErrorCode::incompatible_version, "decode-number", "unsupported-type"));
    }

    [[nodiscard]] core::Result<LegacyValue> read_value() noexcept {
        const auto marker = read_byte();
        if (!marker) {
            return core::Result<LegacyValue>::failure(marker.error());
        }
        if (marker.value() == 0xc2U || marker.value() == 0xc3U) {
            LegacyValue result{};
            result.type = LegacyValueType::boolean;
            result.boolean = marker.value() == 0xc3U;
            return core::Result<LegacyValue>::success(result);
        }
        if ((marker.value() & 0xe0U) == 0xa0U || marker.value() == 0xd9U ||
            marker.value() == 0xdaU) {
            --offset_;
            const auto value = read_string(kMaxStringBytes);
            if (!value) {
                return core::Result<LegacyValue>::failure(value.error());
            }
            LegacyValue result{};
            result.type = LegacyValueType::string;
            result.string = value.value();
            return core::Result<LegacyValue>::success(result);
        }
        if ((marker.value() & 0xf0U) == 0x90U || marker.value() == 0xdcU ||
            marker.value() == 0xddU) {
            const auto count = read_array_count(marker.value());
            if (!count) {
                return core::Result<LegacyValue>::failure(count.error());
            }
            for (std::size_t index = 0; index < count.value(); ++index) {
                const auto item_marker = read_byte();
                if (!item_marker) {
                    return core::Result<LegacyValue>::failure(item_marker.error());
                }
                const auto item = read_number(item_marker.value());
                if (!item) {
                    return core::Result<LegacyValue>::failure(item.error());
                }
                if (item.value().type == LegacyValueType::floating) {
                    workspace_->numeric_array[index] = item.value().floating;
                } else if (item.value().type == LegacyValueType::signed_integer) {
                    if (item.value().signed_integer < -9007199254740991LL ||
                        item.value().signed_integer > 9007199254740991LL) {
                        return core::Result<LegacyValue>::failure(
                            import_error(core::ErrorCode::validation_failed, "decode-array",
                                         "integer-not-exactly-representable"));
                    }
                    workspace_->numeric_array[index] =
                        static_cast<double>(item.value().signed_integer);
                } else if (item.value().type == LegacyValueType::unsigned_integer) {
                    if (item.value().unsigned_integer > 9007199254740991ULL) {
                        return core::Result<LegacyValue>::failure(
                            import_error(core::ErrorCode::validation_failed, "decode-array",
                                         "integer-not-exactly-representable"));
                    }
                    workspace_->numeric_array[index] =
                        static_cast<double>(item.value().unsigned_integer);
                } else {
                    return core::Result<LegacyValue>::failure(
                        import_error(core::ErrorCode::incompatible_version, "decode-array",
                                     "non-numeric-array"));
                }
            }
            LegacyValue result{};
            result.type = LegacyValueType::numeric_array;
            result.numeric_array = {workspace_->numeric_array.data(), count.value()};
            return core::Result<LegacyValue>::success(result);
        }
        return read_number(marker.value());
    }

    [[nodiscard]] bool add_unique(std::span<std::uint64_t> values, std::size_t& size,
                                  std::uint64_t value) noexcept {
        for (std::size_t index = 0; index < size; ++index) {
            if (values[index] == value) {
                return false;
            }
        }
        if (size == values.size()) {
            return false;
        }
        values[size++] = value;
        return true;
    }

    [[nodiscard]] core::Status append_path(std::string_view name,
                                           std::size_t& prior_size) noexcept {
        prior_size = path_size_;
        const std::size_t separator = path_size_ == 0U ? 0U : 1U;
        if (path_size_ + separator + name.size() >= workspace_->component_path.size()) {
            return core::Status::failure(import_error(core::ErrorCode::capacity_exceeded,
                                                      "parse-component", "path-too-long"));
        }
        if (separator != 0U) {
            workspace_->component_path[path_size_++] = '.';
        }
        std::memcpy(workspace_->component_path.data() + path_size_, name.data(), name.size());
        path_size_ += name.size();
        workspace_->component_path[path_size_] = '\0';
        return core::Status::success();
    }

    void restore_path(std::size_t size) noexcept {
        path_size_ = size;
        workspace_->component_path[path_size_] = '\0';
    }

    [[nodiscard]] core::Status parse_component_collection(std::size_t depth) noexcept {
        if (depth > kMaxDepth) {
            return core::Status::failure(import_error(core::ErrorCode::capacity_exceeded,
                                                      "parse-component", "depth-exceeded"));
        }
        const auto count = read_map_count();
        if (!count) {
            return core::Status::failure(count.error());
        }
        for (std::size_t index = 0; index < count.value(); ++index) {
            const auto name = read_string(kMaxNameBytes);
            if (!name) {
                return core::Status::failure(name.error());
            }
            if (!valid_name(name.value())) {
                return core::Status::failure(
                    import_error(core::ErrorCode::corrupt_data, "parse-component", "invalid-name"));
            }
            const auto status = parse_component(name.value(), depth);
            if (!status) {
                return status;
            }
        }
        return core::Status::success();
    }

    [[nodiscard]] core::Status parse_component(std::string_view name, std::size_t depth) noexcept {
        std::size_t prior_size{};
        auto status = append_path(name, prior_size);
        if (!status) {
            return status;
        }
        const std::string_view path{workspace_->component_path.data(), path_size_};
        if (map_legacy_component(path).empty()) {
            restore_path(prior_size);
            return core::Status::failure(import_error(core::ErrorCode::incompatible_version,
                                                      "parse-component", "unmapped-component"));
        }
        if (!add_unique(workspace_->component_hashes, component_count_, hash_text(path))) {
            restore_path(prior_size);
            return core::Status::failure(import_error(core::ErrorCode::duplicate_id,
                                                      "parse-component", "duplicate-component"));
        }
        const auto count = read_map_count();
        if (!count) {
            restore_path(prior_size);
            return core::Status::failure(count.error());
        }
        bool nested_seen{};
        for (std::size_t index = 0; index < count.value(); ++index) {
            const auto field = read_string(kMaxNameBytes);
            if (!field) {
                restore_path(prior_size);
                return core::Status::failure(field.error());
            }
            if (!valid_name(field.value())) {
                restore_path(prior_size);
                return core::Status::failure(
                    import_error(core::ErrorCode::corrupt_data, "parse-field", "invalid-name"));
            }
            if (field.value() == "components") {
                if (nested_seen) {
                    restore_path(prior_size);
                    return core::Status::failure(import_error(
                        core::ErrorCode::duplicate_id, "parse-field", "duplicate-components-key"));
                }
                nested_seen = true;
                status = parse_component_collection(depth + 1U);
                if (!status) {
                    restore_path(prior_size);
                    return status;
                }
                continue;
            }
            if (!add_unique(workspace_->setting_hashes, setting_count_,
                            hash_text(path, field.value()))) {
                restore_path(prior_size);
                return core::Status::failure(import_error(core::ErrorCode::duplicate_id,
                                                          "parse-field", "duplicate-setting"));
            }
            const auto value = read_value();
            if (!value) {
                restore_path(prior_size);
                return core::Status::failure(value.error());
            }
            if (visitor_ != nullptr) {
                const ImportedSetting setting{path, map_legacy_component(path), field.value(),
                                              value.value()};
                status = visitor_(context_, setting);
                if (!status) {
                    restore_path(prior_size);
                    return status;
                }
            }
        }
        restore_path(prior_size);
        return core::Status::success();
    }

    std::span<const std::byte> source_{};
    LegacyImportWorkspace* workspace_{};
    ImportedSettingVisitor visitor_{};
    void* context_{};
    std::size_t offset_{};
    std::size_t path_size_{};
    std::size_t setting_count_{};
    std::size_t component_count_{};
};

} // namespace

std::span<const LegacyComponentMapping> legacy_component_mappings() noexcept { return kMappings; }

std::string_view map_legacy_component(std::string_view legacy_path) noexcept {
    for (const auto& mapping : kMappings) {
        if (mapping.legacy_path == legacy_path) {
            return mapping.component_id;
        }
    }
    return {};
}

core::Result<LegacyImportSummary> LegacySettingsImporter::import(std::span<const std::byte> source,
                                                                 LegacyImportWorkspace& workspace,
                                                                 ImportedSettingVisitor visitor,
                                                                 void* context) const noexcept {
    if (source.empty() || source.size() > kMaxLegacySettingsBytes ||
        (visitor == nullptr && context != nullptr)) {
        return core::Result<LegacyImportSummary>::failure(import_error(
            source.size() > kMaxLegacySettingsBytes ? core::ErrorCode::capacity_exceeded
                                                    : core::ErrorCode::invalid_argument,
            "import", "invalid-source-or-visitor"));
    }
    Parser parser{source, workspace, visitor, context};
    return parser.parse();
}

} // namespace blip::storage
