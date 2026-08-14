#include "blip/storage/settings_store.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

namespace blip::storage {
namespace {

constexpr std::uint32_t kSlotMagic = 0x53504c42U;   // "BLPS" in little-endian bytes.
constexpr std::uint32_t kCommitMagic = 0x43504c42U; // "BLPC" in little-endian bytes.
constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
constexpr std::size_t kCrcOffset = 28;
constexpr std::size_t kCrcBytes = 4;

[[nodiscard]] core::Error settings_error(core::ErrorCode code, std::string_view component,
                                         std::string_view operation,
                                         std::string_view detail) noexcept {
    return {core::ErrorDomain::storage, code, component, operation, detail};
}

[[nodiscard]] core::Error contextualize(const core::Error& error, std::string_view component,
                                        std::string_view operation) noexcept {
    return {error.domain, error.code, component, operation, error.detail};
}

void write_u16(std::span<std::byte> output, std::size_t offset, std::uint16_t value) noexcept {
    output[offset] = static_cast<std::byte>(value & 0xffU);
    output[offset + 1] = static_cast<std::byte>((value >> 8U) & 0xffU);
}

void write_u32(std::span<std::byte> output, std::size_t offset, std::uint32_t value) noexcept {
    for (std::size_t index = 0; index < 4; ++index) {
        output[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
    }
}

void write_u64(std::span<std::byte> output, std::size_t offset, std::uint64_t value) noexcept {
    for (std::size_t index = 0; index < 8; ++index) {
        output[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
    }
}

[[nodiscard]] std::uint16_t read_u16(std::span<const std::byte> input,
                                     std::size_t offset) noexcept {
    return static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(input[offset])) |
           static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(input[offset + 1])) << 8U;
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

[[nodiscard]] std::uint32_t record_crc32(std::span<const std::byte> input) noexcept {
    std::uint32_t crc = 0xffffffffU;
    for (std::size_t index = 0; index < input.size(); ++index) {
        const std::uint8_t byte = index >= kCrcOffset && index < kCrcOffset + kCrcBytes
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

[[nodiscard]] std::string_view key_view(const std::array<char, 14>& key) noexcept {
    return {key.data(), kSettingsNvsKeyBytes};
}

} // namespace

core::Status SettingsStore::validate_descriptor(const core::ComponentDescriptor& descriptor,
                                                std::string_view operation) const noexcept {
    if (descriptor.id.empty() || descriptor.id.size() > kMaxSettingsComponentIdBytes) {
        return core::Status::failure(settings_error(
            core::ErrorCode::invalid_argument, descriptor.id, operation, "component-id-length"));
    }
    if (descriptor.settings.schema_version == 0) {
        return core::Status::failure(settings_error(
            core::ErrorCode::invalid_argument, descriptor.id, operation, "zero-settings-schema"));
    }
    if (scratch_.size() < kSettingsRecordHeaderBytes + descriptor.id.size()) {
        return core::Status::failure(settings_error(core::ErrorCode::capacity_exceeded,
                                                    descriptor.id, operation, "scratch-too-small"));
    }
    return core::Status::success();
}

std::uint64_t SettingsStore::component_hash(std::string_view component_id) noexcept {
    std::uint64_t hash = kFnvOffset;
    for (const char character : component_id) {
        hash ^= static_cast<std::uint8_t>(character);
        hash *= kFnvPrime;
    }
    return hash;
}

SettingsStore::KeySet SettingsStore::make_keys(std::string_view component_id) noexcept {
    KeySet keys{};
    const std::uint64_t hash = component_hash(component_id);
    constexpr char digits[] = "0123456789abcdef";
    const auto fill = [hash, &digits](char prefix,
                                      std::array<char, kKeyBufferBytes>& output) noexcept {
        output[0] = prefix;
        for (std::size_t index = 0; index < 12; ++index) {
            const auto shift = static_cast<unsigned>((11U - index) * 4U);
            output[index + 1] = digits[(hash >> shift) & 0x0fU];
        }
        output[kSettingsNvsKeyBytes] = '\0';
    };
    fill('a', keys.slot_a);
    fill('b', keys.slot_b);
    fill('c', keys.commit);
    return keys;
}

core::Status SettingsStore::parse_slot_record(std::string_view component_id, SettingsSlot slot,
                                              std::span<const std::byte> record,
                                              SlotState& state) const noexcept {
    state = {};
    state.present = true;
    state.slot = slot;
    if (record.size() < kSettingsRecordHeaderBytes) {
        return core::Status::failure(settings_error(core::ErrorCode::corrupt_data, component_id,
                                                    "parse-slot", "short-record"));
    }
    if (read_u32(record, 0) != kSlotMagic) {
        return core::Status::failure(
            settings_error(core::ErrorCode::corrupt_data, component_id, "parse-slot", "bad-magic"));
    }
    if (read_u16(record, 4) != kSettingsRecordFormatVersion ||
        read_u16(record, 6) != kSettingsRecordHeaderBytes) {
        return core::Status::failure(settings_error(core::ErrorCode::incompatible_version,
                                                    component_id, "parse-slot", "record-version"));
    }

    const std::uint32_t schema_version = read_u32(record, 8);
    const std::uint64_t generation = read_u64(record, 12);
    const std::size_t id_size = read_u16(record, 20);
    const std::uint16_t flags = read_u16(record, 22);
    const std::size_t payload_size = read_u32(record, 24);
    if (schema_version == 0 || generation == 0 || id_size == 0 ||
        id_size > kMaxSettingsComponentIdBytes || flags != 0) {
        return core::Status::failure(settings_error(core::ErrorCode::corrupt_data, component_id,
                                                    "parse-slot", "invalid-header"));
    }
    if (id_size > record.size() - kSettingsRecordHeaderBytes ||
        payload_size > record.size() - kSettingsRecordHeaderBytes - id_size ||
        kSettingsRecordHeaderBytes + id_size + payload_size != record.size()) {
        return core::Status::failure(settings_error(core::ErrorCode::corrupt_data, component_id,
                                                    "parse-slot", "invalid-length"));
    }
    const std::string_view stored_id{
        reinterpret_cast<const char*>(record.data() + kSettingsRecordHeaderBytes), id_size};
    if (stored_id != component_id) {
        return core::Status::failure(settings_error(core::ErrorCode::corrupt_data, component_id,
                                                    "parse-slot", "component-mismatch"));
    }
    if (read_u32(record, kCrcOffset) != record_crc32(record)) {
        return core::Status::failure(settings_error(core::ErrorCode::corrupt_data, component_id,
                                                    "parse-slot", "crc-mismatch"));
    }

    state.valid = true;
    state.schema_version = schema_version;
    state.generation = generation;
    state.payload_offset = kSettingsRecordHeaderBytes + id_size;
    state.payload_size = payload_size;
    return core::Status::success();
}

core::Status SettingsStore::parse_commit_record(std::string_view component_id,
                                                std::span<const std::byte> record,
                                                CommitState& state) const noexcept {
    state = {};
    state.present = true;
    if (record.size() != kSettingsCommitRecordBytes) {
        return core::Status::failure(settings_error(core::ErrorCode::corrupt_data, component_id,
                                                    "parse-commit", "invalid-length"));
    }
    if (read_u32(record, 0) != kCommitMagic) {
        return core::Status::failure(settings_error(core::ErrorCode::corrupt_data, component_id,
                                                    "parse-commit", "bad-magic"));
    }
    if (read_u16(record, 4) != kSettingsRecordFormatVersion ||
        read_u16(record, 6) != kSettingsCommitRecordBytes) {
        return core::Status::failure(settings_error(
            core::ErrorCode::incompatible_version, component_id, "parse-commit", "record-version"));
    }
    if (read_u64(record, 8) != component_hash(component_id) || read_u64(record, 16) == 0 ||
        std::to_integer<std::uint8_t>(record[24]) > 1U || record[25] != std::byte{0} ||
        record[26] != std::byte{0} || record[27] != std::byte{0} ||
        read_u32(record, kCrcOffset) != record_crc32(record)) {
        return core::Status::failure(settings_error(core::ErrorCode::corrupt_data, component_id,
                                                    "parse-commit", "invalid-commit"));
    }
    state.valid = true;
    state.generation = read_u64(record, 16);
    state.slot =
        std::to_integer<std::uint8_t>(record[24]) == 0U ? SettingsSlot::a : SettingsSlot::b;
    return core::Status::success();
}

core::Result<SettingsStore::SlotState> SettingsStore::read_slot(std::string_view key,
                                                                std::string_view component_id,
                                                                SettingsSlot slot) noexcept {
    const auto read_result = backend_->read(key, scratch_);
    if (!read_result) {
        if (read_result.error().code == core::ErrorCode::not_found) {
            SlotState missing{};
            missing.slot = slot;
            return core::Result<SlotState>::success(missing);
        }
        return core::Result<SlotState>::failure(
            contextualize(read_result.error(), component_id, "read-slot"));
    }

    SlotState state{};
    const auto parse_status =
        parse_slot_record(component_id, slot,
                          std::span<const std::byte>{scratch_.data(), read_result.value()}, state);
    if (!parse_status) {
        state.present = true;
        state.slot = slot;
    }
    return core::Result<SlotState>::success(state);
}

core::Result<SettingsStore::CommitState>
SettingsStore::read_commit(std::string_view key, std::string_view component_id) noexcept {
    const auto read_result = backend_->read(key, scratch_);
    if (!read_result) {
        if (read_result.error().code == core::ErrorCode::not_found) {
            return core::Result<CommitState>::success({});
        }
        return core::Result<CommitState>::failure(
            contextualize(read_result.error(), component_id, "read-commit"));
    }

    CommitState state{};
    const auto parse_status = parse_commit_record(
        component_id, std::span<const std::byte>{scratch_.data(), read_result.value()}, state);
    if (!parse_status) {
        state.present = true;
    }
    return core::Result<CommitState>::success(state);
}

core::Result<SettingsStore::StoreState>
SettingsStore::inspect(std::string_view component_id) noexcept {
    const KeySet keys = make_keys(component_id);
    StoreState state{};
    const auto slot_a = read_slot(key_view(keys.slot_a), component_id, SettingsSlot::a);
    if (!slot_a) {
        return core::Result<StoreState>::failure(slot_a.error());
    }
    state.slots[0] = slot_a.value();

    const auto slot_b = read_slot(key_view(keys.slot_b), component_id, SettingsSlot::b);
    if (!slot_b) {
        return core::Result<StoreState>::failure(slot_b.error());
    }
    state.slots[1] = slot_b.value();

    const auto commit = read_commit(key_view(keys.commit), component_id);
    if (!commit) {
        return core::Result<StoreState>::failure(commit.error());
    }
    state.commit = commit.value();
    state.any_present = state.slots[0].present || state.slots[1].present || state.commit.present;
    return core::Result<StoreState>::success(state);
}

const SettingsStore::SlotState* SettingsStore::select(const StoreState& state) noexcept {
    if (state.commit.valid) {
        const std::size_t selected_index = state.commit.slot == SettingsSlot::a ? 0U : 1U;
        const SlotState& selected = state.slots[selected_index];
        if (selected.valid && selected.generation == state.commit.generation) {
            return &selected;
        }
    }

    const SlotState* selected{};
    for (const auto& slot : state.slots) {
        if (slot.valid &&
            (selected == nullptr || slot.generation > selected->generation ||
             (slot.generation == selected->generation && slot.slot == SettingsSlot::a))) {
            selected = &slot;
        }
    }
    return selected;
}

core::Status SettingsStore::build_slot_record(std::string_view component_id,
                                              std::uint32_t schema_version,
                                              std::uint64_t generation,
                                              std::span<const std::byte> payload,
                                              std::size_t& record_size) noexcept {
    if (payload.size() > std::numeric_limits<std::uint32_t>::max() ||
        payload.size() > maximum_payload_size(component_id)) {
        return core::Status::failure(settings_error(
            core::ErrorCode::capacity_exceeded, component_id, "build-slot", "payload-too-large"));
    }
    record_size = kSettingsRecordHeaderBytes + component_id.size() + payload.size();
    std::fill_n(scratch_.data(), record_size, std::byte{0});
    write_u32(scratch_, 0, kSlotMagic);
    write_u16(scratch_, 4, kSettingsRecordFormatVersion);
    write_u16(scratch_, 6, static_cast<std::uint16_t>(kSettingsRecordHeaderBytes));
    write_u32(scratch_, 8, schema_version);
    write_u64(scratch_, 12, generation);
    write_u16(scratch_, 20, static_cast<std::uint16_t>(component_id.size()));
    write_u32(scratch_, 24, static_cast<std::uint32_t>(payload.size()));
    std::memcpy(scratch_.data() + kSettingsRecordHeaderBytes, component_id.data(),
                component_id.size());
    if (!payload.empty()) {
        std::memcpy(scratch_.data() + kSettingsRecordHeaderBytes + component_id.size(),
                    payload.data(), payload.size());
    }
    write_u32(scratch_, kCrcOffset,
              record_crc32(std::span<const std::byte>{scratch_.data(), record_size}));
    return core::Status::success();
}

core::Status SettingsStore::build_commit_record(std::string_view component_id, SettingsSlot slot,
                                                std::uint64_t generation) noexcept {
    if (scratch_.size() < kSettingsCommitRecordBytes) {
        return core::Status::failure(settings_error(
            core::ErrorCode::capacity_exceeded, component_id, "build-commit", "scratch-too-small"));
    }
    std::fill_n(scratch_.data(), kSettingsCommitRecordBytes, std::byte{0});
    write_u32(scratch_, 0, kCommitMagic);
    write_u16(scratch_, 4, kSettingsRecordFormatVersion);
    write_u16(scratch_, 6, static_cast<std::uint16_t>(kSettingsCommitRecordBytes));
    write_u64(scratch_, 8, component_hash(component_id));
    write_u64(scratch_, 16, generation);
    scratch_[24] = static_cast<std::byte>(slot);
    write_u32(
        scratch_, kCrcOffset,
        record_crc32(std::span<const std::byte>{scratch_.data(), kSettingsCommitRecordBytes}));
    return core::Status::success();
}

bool SettingsStore::overlaps(std::span<const std::byte> first,
                             std::span<const std::byte> second) noexcept {
    if (first.empty() || second.empty()) {
        return false;
    }
    const auto first_start = reinterpret_cast<std::uintptr_t>(first.data());
    const auto second_start = reinterpret_cast<std::uintptr_t>(second.data());
    return first_start < second_start + second.size() && second_start < first_start + first.size();
}

core::Result<LoadedSettings> SettingsStore::load(const core::ComponentDescriptor& descriptor,
                                                 std::span<std::byte> output) noexcept {
    const auto descriptor_status = validate_descriptor(descriptor, "load");
    if (!descriptor_status) {
        return core::Result<LoadedSettings>::failure(descriptor_status.error());
    }
    if (overlaps(output, std::span<const std::byte>{scratch_.data(), scratch_.size()})) {
        return core::Result<LoadedSettings>::failure(settings_error(
            core::ErrorCode::invalid_argument, descriptor.id, "load", "overlapping-buffer"));
    }

    const auto state_result = inspect(descriptor.id);
    if (!state_result) {
        return core::Result<LoadedSettings>::failure(state_result.error());
    }
    const SlotState* selected = select(state_result.value());
    if (selected == nullptr) {
        const core::ErrorCode code = state_result.value().any_present
                                         ? core::ErrorCode::corrupt_data
                                         : core::ErrorCode::not_found;
        return core::Result<LoadedSettings>::failure(
            settings_error(code, descriptor.id, "load", "no-valid-generation"));
    }
    if (output.size() < selected->payload_size) {
        return core::Result<LoadedSettings>::failure(settings_error(
            core::ErrorCode::capacity_exceeded, descriptor.id, "load", "output-too-small"));
    }

    const KeySet keys = make_keys(descriptor.id);
    const std::string_view key =
        selected->slot == SettingsSlot::a ? key_view(keys.slot_a) : key_view(keys.slot_b);
    const auto read_result = backend_->read(key, scratch_);
    if (!read_result) {
        return core::Result<LoadedSettings>::failure(
            contextualize(read_result.error(), descriptor.id, "load-selected"));
    }
    SlotState verified{};
    const auto parse_status = parse_slot_record(
        descriptor.id, selected->slot,
        std::span<const std::byte>{scratch_.data(), read_result.value()}, verified);
    if (!parse_status || verified.generation != selected->generation) {
        return core::Result<LoadedSettings>::failure(settings_error(
            core::ErrorCode::verification_failed, descriptor.id, "load", "selected-changed"));
    }
    if (verified.payload_size != 0U) {
        std::memcpy(output.data(), scratch_.data() + verified.payload_offset,
                    verified.payload_size);
    }
    return core::Result<LoadedSettings>::success(
        {verified.schema_version, verified.generation, verified.payload_size, verified.slot});
}

core::Status SettingsStore::save(const core::ComponentDescriptor& descriptor,
                                 std::span<const std::byte> payload) noexcept {
    const auto descriptor_status = validate_descriptor(descriptor, "save");
    if (!descriptor_status) {
        return descriptor_status;
    }
    if (overlaps(payload, std::span<const std::byte>{scratch_.data(), scratch_.size()})) {
        return core::Status::failure(settings_error(core::ErrorCode::invalid_argument,
                                                    descriptor.id, "save", "overlapping-buffer"));
    }
    if (payload.size() > maximum_payload_size(descriptor.id)) {
        return core::Status::failure(settings_error(core::ErrorCode::capacity_exceeded,
                                                    descriptor.id, "save", "payload-too-large"));
    }

    const auto state_result = inspect(descriptor.id);
    if (!state_result) {
        return core::Status::failure(state_result.error());
    }
    const StoreState& state = state_result.value();
    std::uint64_t highest_generation{};
    for (const auto& slot : state.slots) {
        if (slot.valid && slot.generation > highest_generation) {
            highest_generation = slot.generation;
        }
    }
    if (highest_generation == std::numeric_limits<std::uint64_t>::max()) {
        return core::Status::failure(settings_error(core::ErrorCode::generation_exhausted,
                                                    descriptor.id, "save", "generation-exhausted"));
    }

    const SlotState* active = select(state);
    const SettingsSlot target =
        active == nullptr || active->slot == SettingsSlot::b ? SettingsSlot::a : SettingsSlot::b;
    const std::uint64_t generation = highest_generation + 1U;
    std::size_t record_size{};
    const auto build_status = build_slot_record(descriptor.id, descriptor.settings.schema_version,
                                                generation, payload, record_size);
    if (!build_status) {
        return build_status;
    }

    const KeySet keys = make_keys(descriptor.id);
    const std::string_view slot_key =
        target == SettingsSlot::a ? key_view(keys.slot_a) : key_view(keys.slot_b);
    auto write_status =
        backend_->write(slot_key, std::span<const std::byte>{scratch_.data(), record_size});
    if (!write_status) {
        return core::Status::failure(
            contextualize(write_status.error(), descriptor.id, "write-slot"));
    }

    const auto verified_slot = read_slot(slot_key, descriptor.id, target);
    if (!verified_slot || !verified_slot.value().valid ||
        verified_slot.value().schema_version != descriptor.settings.schema_version ||
        verified_slot.value().generation != generation ||
        verified_slot.value().payload_size != payload.size() ||
        (!payload.empty() && std::memcmp(scratch_.data() + verified_slot.value().payload_offset,
                                         payload.data(), payload.size()) != 0)) {
        return core::Status::failure(settings_error(core::ErrorCode::verification_failed,
                                                    descriptor.id, "verify-slot",
                                                    "readback-mismatch"));
    }

    const auto commit_status = build_commit_record(descriptor.id, target, generation);
    if (!commit_status) {
        return commit_status;
    }
    write_status =
        backend_->write(key_view(keys.commit),
                        std::span<const std::byte>{scratch_.data(), kSettingsCommitRecordBytes});
    if (!write_status) {
        return core::Status::failure(
            contextualize(write_status.error(), descriptor.id, "write-commit"));
    }

    const auto verified_commit = read_commit(key_view(keys.commit), descriptor.id);
    if (!verified_commit || !verified_commit.value().valid ||
        verified_commit.value().slot != target ||
        verified_commit.value().generation != generation) {
        return core::Status::failure(settings_error(core::ErrorCode::verification_failed,
                                                    descriptor.id, "verify-commit",
                                                    "readback-mismatch"));
    }
    return core::Status::success();
}

std::size_t SettingsStore::maximum_payload_size(std::string_view component_id) const noexcept {
    const std::size_t overhead = kSettingsRecordHeaderBytes + component_id.size();
    return scratch_.size() > overhead ? scratch_.size() - overhead : 0U;
}

} // namespace blip::storage
