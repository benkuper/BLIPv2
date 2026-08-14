#include "blip/storage/atomic_file_store.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

namespace blip::storage {
namespace {

constexpr std::uint32_t kEncodedFileMagic = 0x46494c42U;    // "BLIF".
constexpr std::uint32_t kEncodedPointerMagic = 0x50464c42U; // "BLFP".
constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
constexpr std::size_t kCrcOffset = 28;
constexpr std::size_t kCrcBytes = 4;

[[nodiscard]] core::Error file_error(core::ErrorCode code, std::string_view path,
                                     std::string_view operation, std::string_view detail) noexcept {
    return {core::ErrorDomain::storage, code, path, operation, detail};
}

[[nodiscard]] core::Error contextualize(const core::Error& error, std::string_view path,
                                        std::string_view operation) noexcept {
    return {error.domain, error.code, path, operation, error.detail};
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

} // namespace

core::Status AtomicFileStore::validate_path(std::string_view logical_path,
                                            std::string_view operation) const noexcept {
    if (logical_path.empty() || logical_path.size() > kMaxLogicalPathBytes ||
        logical_path.front() == '/' || logical_path.ends_with('/') ||
        logical_path.find("..") != std::string_view::npos) {
        return core::Status::failure(file_error(core::ErrorCode::invalid_argument, logical_path,
                                                operation, "invalid-logical-path"));
    }
    for (const char character : logical_path) {
        const bool valid = (character >= 'a' && character <= 'z') ||
                           (character >= 'A' && character <= 'Z') ||
                           (character >= '0' && character <= '9') || character == '/' ||
                           character == '_' || character == '-' || character == '.';
        if (!valid) {
            return core::Status::failure(file_error(core::ErrorCode::invalid_argument, logical_path,
                                                    operation, "invalid-path-character"));
        }
    }
    if (scratch_.size() < kAtomicFileHeaderBytes + logical_path.size()) {
        return core::Status::failure(file_error(core::ErrorCode::capacity_exceeded, logical_path,
                                                operation, "scratch-too-small"));
    }
    return core::Status::success();
}

std::uint64_t AtomicFileStore::path_hash(std::string_view logical_path) noexcept {
    std::uint64_t hash = kFnvOffset;
    for (const char character : logical_path) {
        hash ^= static_cast<std::uint8_t>(character);
        hash *= kFnvPrime;
    }
    return hash;
}

bool AtomicFileStore::make_path(std::string_view logical_path, std::string_view suffix,
                                std::span<char> output, std::string_view& result) noexcept {
    if (logical_path.size() + suffix.size() + 1U > output.size()) {
        return false;
    }
    std::memcpy(output.data(), logical_path.data(), logical_path.size());
    std::memcpy(output.data() + logical_path.size(), suffix.data(), suffix.size());
    output[logical_path.size() + suffix.size()] = '\0';
    result = {output.data(), logical_path.size() + suffix.size()};
    return true;
}

core::Status AtomicFileStore::parse_record(std::string_view logical_path, FileSlot slot,
                                           std::span<const std::byte> record,
                                           FileState& state) const noexcept {
    state = {};
    state.present = true;
    state.slot = slot;
    if (record.size() < kAtomicFileHeaderBytes) {
        return core::Status::failure(
            file_error(core::ErrorCode::corrupt_data, logical_path, "parse-file", "short-record"));
    }
    if (read_u32(record, 0) != kEncodedFileMagic) {
        return core::Status::failure(
            file_error(core::ErrorCode::corrupt_data, logical_path, "parse-file", "bad-magic"));
    }
    if (read_u16(record, 4) != kAtomicFileFormatVersion ||
        read_u16(record, 6) != kAtomicFileHeaderBytes) {
        return core::Status::failure(file_error(core::ErrorCode::incompatible_version, logical_path,
                                                "parse-file", "record-version"));
    }
    const std::uint64_t generation = read_u64(record, 8);
    const std::size_t path_size = read_u16(record, 16);
    const std::uint16_t flags = read_u16(record, 18);
    const std::size_t payload_size = read_u32(record, 20);
    if (generation == 0 || path_size == 0 || path_size > kMaxLogicalPathBytes || flags != 0 ||
        read_u32(record, 24) != 0) {
        return core::Status::failure(file_error(core::ErrorCode::corrupt_data, logical_path,
                                                "parse-file", "invalid-header"));
    }
    if (path_size > record.size() - kAtomicFileHeaderBytes ||
        payload_size > record.size() - kAtomicFileHeaderBytes - path_size ||
        kAtomicFileHeaderBytes + path_size + payload_size != record.size()) {
        return core::Status::failure(file_error(core::ErrorCode::corrupt_data, logical_path,
                                                "parse-file", "invalid-length"));
    }
    const std::string_view stored_path{
        reinterpret_cast<const char*>(record.data() + kAtomicFileHeaderBytes), path_size};
    if (stored_path != logical_path || read_u32(record, kCrcOffset) != record_crc32(record)) {
        return core::Status::failure(file_error(core::ErrorCode::corrupt_data, logical_path,
                                                "parse-file", "identity-or-crc"));
    }
    state.valid = true;
    state.generation = generation;
    state.payload_offset = kAtomicFileHeaderBytes + path_size;
    state.payload_size = payload_size;
    return core::Status::success();
}

core::Status AtomicFileStore::parse_pointer(std::string_view logical_path,
                                            std::span<const std::byte> record,
                                            PointerState& state) const noexcept {
    state = {};
    state.present = true;
    if (record.size() != kAtomicFilePointerBytes || read_u32(record, 0) != kEncodedPointerMagic) {
        return core::Status::failure(file_error(core::ErrorCode::corrupt_data, logical_path,
                                                "parse-pointer", "invalid-record"));
    }
    if (read_u16(record, 4) != kAtomicFileFormatVersion ||
        read_u16(record, 6) != kAtomicFilePointerBytes) {
        return core::Status::failure(file_error(core::ErrorCode::incompatible_version, logical_path,
                                                "parse-pointer", "record-version"));
    }
    if (read_u64(record, 8) != path_hash(logical_path) || read_u64(record, 16) == 0 ||
        std::to_integer<std::uint8_t>(record[24]) > 1U || record[25] != std::byte{0} ||
        record[26] != std::byte{0} || record[27] != std::byte{0} ||
        read_u32(record, kCrcOffset) != record_crc32(record)) {
        return core::Status::failure(file_error(core::ErrorCode::corrupt_data, logical_path,
                                                "parse-pointer", "invalid-pointer"));
    }
    state.valid = true;
    state.generation = read_u64(record, 16);
    state.slot = std::to_integer<std::uint8_t>(record[24]) == 0U ? FileSlot::a : FileSlot::b;
    return core::Status::success();
}

core::Result<AtomicFileStore::FileState> AtomicFileStore::read_record(std::string_view storage_path,
                                                                      std::string_view logical_path,
                                                                      FileSlot slot) noexcept {
    const auto read_result = backend_->read(storage_path, scratch_);
    if (!read_result) {
        if (read_result.error().code == core::ErrorCode::not_found) {
            FileState missing{};
            missing.slot = slot;
            return core::Result<FileState>::success(missing);
        }
        return core::Result<FileState>::failure(
            contextualize(read_result.error(), logical_path, "read-file"));
    }
    FileState state{};
    const auto parsed =
        parse_record(logical_path, slot,
                     std::span<const std::byte>{scratch_.data(), read_result.value()}, state);
    if (!parsed) {
        state.present = true;
        state.slot = slot;
    }
    return core::Result<FileState>::success(state);
}

core::Result<AtomicFileStore::PointerState>
AtomicFileStore::read_pointer(std::string_view storage_path,
                              std::string_view logical_path) noexcept {
    const auto read_result = backend_->read(storage_path, scratch_);
    if (!read_result) {
        if (read_result.error().code == core::ErrorCode::not_found) {
            return core::Result<PointerState>::success({});
        }
        return core::Result<PointerState>::failure(
            contextualize(read_result.error(), logical_path, "read-pointer"));
    }
    PointerState state{};
    const auto parsed = parse_pointer(
        logical_path, std::span<const std::byte>{scratch_.data(), read_result.value()}, state);
    if (!parsed) {
        state.present = true;
    }
    return core::Result<PointerState>::success(state);
}

core::Result<AtomicFileStore::SlotSet>
AtomicFileStore::inspect_slots(std::string_view logical_path) noexcept {
    std::array<char, kPathBufferBytes> path_a{};
    std::array<char, kPathBufferBytes> path_b{};
    std::array<char, kPathBufferBytes> pointer_path{};
    std::string_view a{};
    std::string_view b{};
    std::string_view pointer{};
    if (!make_path(logical_path, ".a", path_a, a) || !make_path(logical_path, ".b", path_b, b) ||
        !make_path(logical_path, ".ptr", pointer_path, pointer)) {
        return core::Result<SlotSet>::failure(file_error(core::ErrorCode::capacity_exceeded,
                                                         logical_path, "inspect", "path-too-long"));
    }
    SlotSet state{};
    const auto slot_a = read_record(a, logical_path, FileSlot::a);
    if (!slot_a) {
        return core::Result<SlotSet>::failure(slot_a.error());
    }
    state.slots[0] = slot_a.value();
    const auto slot_b = read_record(b, logical_path, FileSlot::b);
    if (!slot_b) {
        return core::Result<SlotSet>::failure(slot_b.error());
    }
    state.slots[1] = slot_b.value();
    const auto pointer_result = read_pointer(pointer, logical_path);
    if (!pointer_result) {
        return core::Result<SlotSet>::failure(pointer_result.error());
    }
    state.pointer = pointer_result.value();
    state.any_present = state.slots[0].present || state.slots[1].present || state.pointer.present;
    return core::Result<SlotSet>::success(state);
}

const AtomicFileStore::FileState* AtomicFileStore::select(const SlotSet& state) noexcept {
    if (state.pointer.valid) {
        const auto index = state.pointer.slot == FileSlot::a ? 0U : 1U;
        if (state.slots[index].valid && state.slots[index].generation == state.pointer.generation) {
            return &state.slots[index];
        }
    }
    const FileState* selected{};
    for (const auto& slot : state.slots) {
        if (slot.valid && (selected == nullptr || slot.generation > selected->generation ||
                           (slot.generation == selected->generation && slot.slot == FileSlot::a))) {
            selected = &slot;
        }
    }
    return selected;
}

core::Status AtomicFileStore::build_record(std::string_view logical_path, std::uint64_t generation,
                                           std::span<const std::byte> payload,
                                           std::size_t& record_size) noexcept {
    if (payload.size() > std::numeric_limits<std::uint32_t>::max() ||
        payload.size() > maximum_payload_size(logical_path)) {
        return core::Status::failure(file_error(core::ErrorCode::capacity_exceeded, logical_path,
                                                "build-file", "payload-too-large"));
    }
    record_size = kAtomicFileHeaderBytes + logical_path.size() + payload.size();
    std::fill_n(scratch_.data(), record_size, std::byte{0});
    write_u32(scratch_, 0, kEncodedFileMagic);
    write_u16(scratch_, 4, kAtomicFileFormatVersion);
    write_u16(scratch_, 6, static_cast<std::uint16_t>(kAtomicFileHeaderBytes));
    write_u64(scratch_, 8, generation);
    write_u16(scratch_, 16, static_cast<std::uint16_t>(logical_path.size()));
    write_u32(scratch_, 20, static_cast<std::uint32_t>(payload.size()));
    std::memcpy(scratch_.data() + kAtomicFileHeaderBytes, logical_path.data(), logical_path.size());
    if (!payload.empty()) {
        std::memcpy(scratch_.data() + kAtomicFileHeaderBytes + logical_path.size(), payload.data(),
                    payload.size());
    }
    write_u32(scratch_, kCrcOffset,
              record_crc32(std::span<const std::byte>{scratch_.data(), record_size}));
    return core::Status::success();
}

core::Status AtomicFileStore::build_pointer(std::string_view logical_path, FileSlot slot,
                                            std::uint64_t generation) noexcept {
    if (scratch_.size() < kAtomicFilePointerBytes) {
        return core::Status::failure(file_error(core::ErrorCode::capacity_exceeded, logical_path,
                                                "build-pointer", "scratch-too-small"));
    }
    std::fill_n(scratch_.data(), kAtomicFilePointerBytes, std::byte{0});
    write_u32(scratch_, 0, kEncodedPointerMagic);
    write_u16(scratch_, 4, kAtomicFileFormatVersion);
    write_u16(scratch_, 6, static_cast<std::uint16_t>(kAtomicFilePointerBytes));
    write_u64(scratch_, 8, path_hash(logical_path));
    write_u64(scratch_, 16, generation);
    scratch_[24] = static_cast<std::byte>(slot);
    write_u32(scratch_, kCrcOffset,
              record_crc32(std::span<const std::byte>{scratch_.data(), kAtomicFilePointerBytes}));
    return core::Status::success();
}

bool AtomicFileStore::overlaps(std::span<const std::byte> first,
                               std::span<const std::byte> second) noexcept {
    if (first.empty() || second.empty()) {
        return false;
    }
    const auto first_start = reinterpret_cast<std::uintptr_t>(first.data());
    const auto second_start = reinterpret_cast<std::uintptr_t>(second.data());
    return first_start < second_start + second.size() && second_start < first_start + first.size();
}

core::Result<LoadedFile> AtomicFileStore::load_rename(std::string_view logical_path,
                                                      std::span<std::byte> output) noexcept {
    const auto record = read_record(logical_path, logical_path, FileSlot::a);
    if (!record) {
        return core::Result<LoadedFile>::failure(record.error());
    }
    if (!record.value().present) {
        return core::Result<LoadedFile>::failure(
            file_error(core::ErrorCode::not_found, logical_path, "load", "missing-file"));
    }
    if (!record.value().valid) {
        return core::Result<LoadedFile>::failure(
            file_error(core::ErrorCode::corrupt_data, logical_path, "load", "corrupt-file"));
    }
    if (output.size() < record.value().payload_size) {
        return core::Result<LoadedFile>::failure(file_error(
            core::ErrorCode::capacity_exceeded, logical_path, "load", "output-too-small"));
    }
    if (record.value().payload_size != 0U) {
        std::memcpy(output.data(), scratch_.data() + record.value().payload_offset,
                    record.value().payload_size);
    }
    return core::Result<LoadedFile>::success(
        {record.value().generation, record.value().payload_size, FileSlot::a});
}

core::Result<LoadedFile> AtomicFileStore::load_slots(std::string_view logical_path,
                                                     std::span<std::byte> output) noexcept {
    const auto state = inspect_slots(logical_path);
    if (!state) {
        return core::Result<LoadedFile>::failure(state.error());
    }
    const FileState* selected = select(state.value());
    if (selected == nullptr) {
        return core::Result<LoadedFile>::failure(file_error(
            state.value().any_present ? core::ErrorCode::corrupt_data : core::ErrorCode::not_found,
            logical_path, "load", "no-valid-generation"));
    }
    if (output.size() < selected->payload_size) {
        return core::Result<LoadedFile>::failure(file_error(
            core::ErrorCode::capacity_exceeded, logical_path, "load", "output-too-small"));
    }
    std::array<char, kPathBufferBytes> selected_path_buffer{};
    std::string_view selected_path{};
    if (!make_path(logical_path, selected->slot == FileSlot::a ? ".a" : ".b", selected_path_buffer,
                   selected_path)) {
        return core::Result<LoadedFile>::failure(
            file_error(core::ErrorCode::capacity_exceeded, logical_path, "load", "path-too-long"));
    }
    const auto verified = read_record(selected_path, logical_path, selected->slot);
    if (!verified || !verified.value().valid ||
        verified.value().generation != selected->generation) {
        return core::Result<LoadedFile>::failure(file_error(
            core::ErrorCode::verification_failed, logical_path, "load", "selected-changed"));
    }
    if (verified.value().payload_size != 0U) {
        std::memcpy(output.data(), scratch_.data() + verified.value().payload_offset,
                    verified.value().payload_size);
    }
    return core::Result<LoadedFile>::success(
        {verified.value().generation, verified.value().payload_size, verified.value().slot});
}

core::Result<LoadedFile> AtomicFileStore::load(std::string_view logical_path,
                                               std::span<std::byte> output) noexcept {
    const auto valid = validate_path(logical_path, "load");
    if (!valid) {
        return core::Result<LoadedFile>::failure(valid.error());
    }
    if (overlaps(output, std::span<const std::byte>{scratch_.data(), scratch_.size()})) {
        return core::Result<LoadedFile>::failure(file_error(
            core::ErrorCode::invalid_argument, logical_path, "load", "overlapping-buffer"));
    }
    return policy_ == FileCommitPolicy::atomic_rename ? load_rename(logical_path, output)
                                                      : load_slots(logical_path, output);
}

core::Status AtomicFileStore::save_rename(std::string_view logical_path,
                                          std::span<const std::byte> payload) noexcept {
    const auto existing = read_record(logical_path, logical_path, FileSlot::a);
    if (!existing) {
        return core::Status::failure(existing.error());
    }
    if (existing.value().present && !existing.value().valid) {
        return core::Status::failure(
            file_error(core::ErrorCode::corrupt_data, logical_path, "save", "corrupt-current"));
    }
    if (existing.value().valid &&
        existing.value().generation == std::numeric_limits<std::uint64_t>::max()) {
        return core::Status::failure(file_error(core::ErrorCode::generation_exhausted, logical_path,
                                                "save", "generation-exhausted"));
    }
    const std::uint64_t generation = existing.value().valid ? existing.value().generation + 1U : 1U;
    std::size_t record_size{};
    auto status = build_record(logical_path, generation, payload, record_size);
    if (!status) {
        return status;
    }
    std::array<char, kPathBufferBytes> temp_buffer{};
    std::string_view temp_path{};
    if (!make_path(logical_path, ".tmp", temp_buffer, temp_path)) {
        return core::Status::failure(
            file_error(core::ErrorCode::capacity_exceeded, logical_path, "save", "path-too-long"));
    }
    status = backend_->write_durable(temp_path,
                                     std::span<const std::byte>{scratch_.data(), record_size});
    if (!status) {
        return core::Status::failure(contextualize(status.error(), logical_path, "write-temp"));
    }
    const auto verified = read_record(temp_path, logical_path, FileSlot::a);
    if (!verified || !verified.value().valid || verified.value().generation != generation ||
        verified.value().payload_size != payload.size() ||
        (!payload.empty() && std::memcmp(scratch_.data() + verified.value().payload_offset,
                                         payload.data(), payload.size()) != 0)) {
        return core::Status::failure(file_error(core::ErrorCode::verification_failed, logical_path,
                                                "verify-temp", "readback-mismatch"));
    }
    status = backend_->replace(temp_path, logical_path);
    if (!status) {
        return core::Status::failure(contextualize(status.error(), logical_path, "replace"));
    }
    const auto committed = read_record(logical_path, logical_path, FileSlot::a);
    if (!committed || !committed.value().valid || committed.value().generation != generation) {
        return core::Status::failure(file_error(core::ErrorCode::verification_failed, logical_path,
                                                "verify-committed", "readback-mismatch"));
    }
    return core::Status::success();
}

core::Status AtomicFileStore::save_slots(std::string_view logical_path,
                                         std::span<const std::byte> payload) noexcept {
    const auto inspected = inspect_slots(logical_path);
    if (!inspected) {
        return core::Status::failure(inspected.error());
    }
    std::uint64_t highest{};
    for (const auto& slot : inspected.value().slots) {
        if (slot.valid && slot.generation > highest) {
            highest = slot.generation;
        }
    }
    if (highest == std::numeric_limits<std::uint64_t>::max()) {
        return core::Status::failure(file_error(core::ErrorCode::generation_exhausted, logical_path,
                                                "save", "generation-exhausted"));
    }
    const FileState* active = select(inspected.value());
    const FileSlot target =
        active == nullptr || active->slot == FileSlot::b ? FileSlot::a : FileSlot::b;
    const std::uint64_t generation = highest + 1U;
    std::size_t record_size{};
    auto status = build_record(logical_path, generation, payload, record_size);
    if (!status) {
        return status;
    }
    std::array<char, kPathBufferBytes> target_buffer{};
    std::string_view target_path{};
    if (!make_path(logical_path, target == FileSlot::a ? ".a" : ".b", target_buffer, target_path)) {
        return core::Status::failure(
            file_error(core::ErrorCode::capacity_exceeded, logical_path, "save", "path-too-long"));
    }
    status = backend_->write_durable(target_path,
                                     std::span<const std::byte>{scratch_.data(), record_size});
    if (!status) {
        return core::Status::failure(contextualize(status.error(), logical_path, "write-slot"));
    }
    const auto verified = read_record(target_path, logical_path, target);
    if (!verified || !verified.value().valid || verified.value().generation != generation ||
        verified.value().payload_size != payload.size() ||
        (!payload.empty() && std::memcmp(scratch_.data() + verified.value().payload_offset,
                                         payload.data(), payload.size()) != 0)) {
        return core::Status::failure(file_error(core::ErrorCode::verification_failed, logical_path,
                                                "verify-slot", "readback-mismatch"));
    }
    status = build_pointer(logical_path, target, generation);
    if (!status) {
        return status;
    }
    std::array<char, kPathBufferBytes> pointer_buffer{};
    std::string_view pointer_path{};
    if (!make_path(logical_path, ".ptr", pointer_buffer, pointer_path)) {
        return core::Status::failure(
            file_error(core::ErrorCode::capacity_exceeded, logical_path, "save", "path-too-long"));
    }
    status = backend_->write_durable(
        pointer_path, std::span<const std::byte>{scratch_.data(), kAtomicFilePointerBytes});
    if (!status) {
        return core::Status::failure(contextualize(status.error(), logical_path, "write-pointer"));
    }
    const auto pointer = read_pointer(pointer_path, logical_path);
    if (!pointer || !pointer.value().valid || pointer.value().slot != target ||
        pointer.value().generation != generation) {
        return core::Status::failure(file_error(core::ErrorCode::verification_failed, logical_path,
                                                "verify-pointer", "readback-mismatch"));
    }
    return core::Status::success();
}

core::Status AtomicFileStore::save(std::string_view logical_path,
                                   std::span<const std::byte> payload) noexcept {
    const auto valid = validate_path(logical_path, "save");
    if (!valid) {
        return valid;
    }
    if (overlaps(payload, std::span<const std::byte>{scratch_.data(), scratch_.size()})) {
        return core::Status::failure(file_error(core::ErrorCode::invalid_argument, logical_path,
                                                "save", "overlapping-buffer"));
    }
    if (payload.size() > maximum_payload_size(logical_path)) {
        return core::Status::failure(file_error(core::ErrorCode::capacity_exceeded, logical_path,
                                                "save", "payload-too-large"));
    }
    return policy_ == FileCommitPolicy::atomic_rename ? save_rename(logical_path, payload)
                                                      : save_slots(logical_path, payload);
}

std::size_t AtomicFileStore::maximum_payload_size(std::string_view logical_path) const noexcept {
    const std::size_t overhead = kAtomicFileHeaderBytes + logical_path.size();
    return scratch_.size() > overhead ? scratch_.size() - overhead : 0U;
}

} // namespace blip::storage
