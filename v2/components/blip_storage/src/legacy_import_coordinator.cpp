#include "blip/storage/legacy_import_coordinator.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace blip::storage {
namespace {

constexpr std::uint32_t kJournalMagic = 0x49504c42U; // "BLPI".
constexpr std::uint16_t kJournalFormatVersion = 1;
constexpr std::size_t kJournalCrcOffset = 60;
constexpr std::string_view kJournalComponentId = "blip.storage.legacy-import";

enum class JournalState : std::uint8_t { pending = 1, confirmed = 2 };

struct Journal {
    std::uint32_t importer_version{};
    JournalState state{JournalState::pending};
    std::uint16_t setting_count{};
    std::uint16_t component_count{};
    std::uint16_t preserved_count{};
    std::array<std::byte, 32> source_sha256{};
};

struct BuildContext {
    std::string_view component_id{};
    ImportedSettingsBuilder* builder{};
};

[[nodiscard]] core::Error coordinator_error(core::ErrorCode code, std::string_view operation,
                                            std::string_view detail) noexcept {
    return {core::ErrorDomain::storage, code, kJournalComponentId, operation, detail};
}

[[nodiscard]] core::ComponentDescriptor descriptor_for(std::string_view component_id) noexcept {
    core::ComponentDescriptor descriptor{};
    descriptor.schema_version = 1;
    descriptor.id = component_id;
    descriptor.display_name = component_id;
    descriptor.description = "Imported V1 settings";
    descriptor.settings = {1, 1};
    return descriptor;
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

[[nodiscard]] std::uint32_t journal_crc(std::span<const std::byte> input) noexcept {
    std::uint32_t crc = 0xffffffffU;
    for (std::size_t index = 0; index < input.size(); ++index) {
        const std::uint8_t byte = index >= kJournalCrcOffset && index < kJournalCrcOffset + 4U
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

[[nodiscard]] core::Status encode_journal(const Journal& journal,
                                          std::span<std::byte> output) noexcept {
    if (output.size() < kLegacyImportJournalBytes) {
        return core::Status::failure(coordinator_error(core::ErrorCode::capacity_exceeded,
                                                       "encode-journal", "buffer-too-small"));
    }
    std::fill_n(output.data(), kLegacyImportJournalBytes, std::byte{0});
    write_u32(output, 0, kJournalMagic);
    write_u16(output, 4, kJournalFormatVersion);
    write_u16(output, 6, static_cast<std::uint16_t>(kLegacyImportJournalBytes));
    write_u32(output, 8, journal.importer_version);
    output[12] = static_cast<std::byte>(journal.state);
    write_u16(output, 16, journal.setting_count);
    write_u16(output, 18, journal.component_count);
    write_u16(output, 20, journal.preserved_count);
    std::memcpy(output.data() + 24U, journal.source_sha256.data(), journal.source_sha256.size());
    write_u32(output, kJournalCrcOffset, journal_crc(output.first(kLegacyImportJournalBytes)));
    return core::Status::success();
}

[[nodiscard]] core::Result<Journal> decode_journal(std::span<const std::byte> input) noexcept {
    if (input.size() != kLegacyImportJournalBytes || read_u32(input, 0) != kJournalMagic ||
        read_u16(input, 4) != kJournalFormatVersion ||
        read_u16(input, 6) != kLegacyImportJournalBytes || input[13] != std::byte{0} ||
        input[14] != std::byte{0} || input[15] != std::byte{0} || input[22] != std::byte{0} ||
        input[23] != std::byte{0} || input[56] != std::byte{0} || input[57] != std::byte{0} ||
        input[58] != std::byte{0} || input[59] != std::byte{0} ||
        read_u32(input, kJournalCrcOffset) != journal_crc(input)) {
        return core::Result<Journal>::failure(
            coordinator_error(core::ErrorCode::corrupt_data, "decode-journal", "invalid-record"));
    }
    const auto state = static_cast<JournalState>(std::to_integer<std::uint8_t>(input[12]));
    if (state != JournalState::pending && state != JournalState::confirmed) {
        return core::Result<Journal>::failure(
            coordinator_error(core::ErrorCode::corrupt_data, "decode-journal", "invalid-state"));
    }
    Journal journal{};
    journal.importer_version = read_u32(input, 8);
    journal.state = state;
    journal.setting_count = read_u16(input, 16);
    journal.component_count = read_u16(input, 18);
    journal.preserved_count = read_u16(input, 20);
    std::memcpy(journal.source_sha256.data(), input.data() + 24U, journal.source_sha256.size());
    return core::Result<Journal>::success(journal);
}

[[nodiscard]] core::Status build_setting(void* context, const ImportedSetting& setting) noexcept {
    auto& build = *static_cast<BuildContext*>(context);
    if (setting.component_id != build.component_id) {
        return core::Status::success();
    }
    return build.builder->append(setting);
}

} // namespace

core::Result<LegacyImportResult> LegacyImportCoordinator::run() noexcept {
    if (settings_ == nullptr || source_ == nullptr || workspace_ == nullptr ||
        source_buffer_.size() < kMaxLegacySettingsBytes ||
        payload_buffer_.size() < kLegacyImportJournalBytes ||
        readback_buffer_.size() < kLegacyImportJournalBytes) {
        return core::Result<LegacyImportResult>::failure(coordinator_error(
            core::ErrorCode::invalid_argument, "run-import", "invalid-coordinator-buffers"));
    }
    const auto source_read = source_->read(source_buffer_);
    if (!source_read) {
        if (source_read.error().code == core::ErrorCode::not_found) {
            return core::Result<LegacyImportResult>::success(
                {LegacyImportDisposition::no_source, 0, 0, 0});
        }
        return core::Result<LegacyImportResult>::failure(source_read.error());
    }
    const auto source = std::span<const std::byte>{source_buffer_.data(), source_read.value()};
    LegacySettingsImporter importer{};
    const auto validated = importer.import(source, *workspace_);
    if (!validated) {
        return core::Result<LegacyImportResult>::failure(validated.error());
    }

    const core::ComponentDescriptor journal_descriptor = descriptor_for(kJournalComponentId);
    const auto journal_loaded = settings_->load(journal_descriptor, readback_buffer_);
    bool journal_present{};
    Journal prior_journal{};
    if (journal_loaded) {
        const auto decoded = decode_journal(std::span<const std::byte>{
            readback_buffer_.data(), journal_loaded.value().payload_size});
        if (!decoded) {
            return core::Result<LegacyImportResult>::failure(decoded.error());
        }
        prior_journal = decoded.value();
        journal_present = true;
        if (prior_journal.source_sha256 != validated.value().source_sha256) {
            return core::Result<LegacyImportResult>::failure(coordinator_error(
                core::ErrorCode::incompatible_version, "run-import", "source-hash-changed"));
        }
        if (prior_journal.importer_version > kLegacySettingsImporterVersion) {
            return core::Result<LegacyImportResult>::failure(coordinator_error(
                core::ErrorCode::incompatible_version, "run-import", "newer-importer-journal"));
        }
        if (prior_journal.importer_version == kLegacySettingsImporterVersion &&
            prior_journal.state == JournalState::confirmed) {
            return core::Result<LegacyImportResult>::success(
                {LegacyImportDisposition::already_confirmed, 0, prior_journal.component_count,
                 prior_journal.setting_count});
        }
    } else if (journal_loaded.error().code != core::ErrorCode::not_found) {
        return core::Result<LegacyImportResult>::failure(journal_loaded.error());
    }

    LegacyImportResult result{LegacyImportDisposition::imported_pending, 0, 0,
                              validated.value().setting_count};
    const auto mappings = legacy_component_mappings();
    for (std::size_t mapping_index = 0; mapping_index < mappings.size(); ++mapping_index) {
        const auto& mapping = mappings[mapping_index];
        bool target_seen{};
        for (std::size_t prior = 0; prior < mapping_index; ++prior) {
            target_seen |= mappings[prior].component_id == mapping.component_id;
        }
        if (target_seen) {
            continue; // Multiple V1 paths can feed one V2 component.
        }
        ImportedSettingsBuilder builder{payload_buffer_};
        BuildContext context{mapping.component_id, &builder};
        const auto imported = importer.import(source, *workspace_, build_setting, &context);
        if (!imported) {
            return core::Result<LegacyImportResult>::failure(imported.error());
        }
        if (builder.field_count() == 0U) {
            continue;
        }
        const auto encoded = builder.finish();
        if (!encoded) {
            return core::Result<LegacyImportResult>::failure(encoded.error());
        }
        const core::ComponentDescriptor target_descriptor = descriptor_for(mapping.component_id);
        const auto existing = settings_->load(target_descriptor, readback_buffer_);
        if (existing || existing.error().code == core::ErrorCode::capacity_exceeded) {
            ++result.preserved_components;
            continue;
        }
        if (existing.error().code != core::ErrorCode::not_found) {
            return core::Result<LegacyImportResult>::failure(existing.error());
        }
        const auto saved = settings_->save(
            target_descriptor, std::span<const std::byte>{payload_buffer_.data(), encoded.value()});
        if (!saved) {
            return core::Result<LegacyImportResult>::failure(saved.error());
        }
        ++result.imported_components;
    }

    if (journal_present && prior_journal.importer_version == kLegacySettingsImporterVersion &&
        prior_journal.state == JournalState::pending) {
        result.disposition = LegacyImportDisposition::already_pending;
        return core::Result<LegacyImportResult>::success(result);
    }
    Journal journal{};
    journal.importer_version = kLegacySettingsImporterVersion;
    journal.state = JournalState::pending;
    journal.setting_count = static_cast<std::uint16_t>(validated.value().setting_count);
    journal.component_count = static_cast<std::uint16_t>(validated.value().component_count);
    journal.preserved_count = static_cast<std::uint16_t>(result.preserved_components);
    journal.source_sha256 = validated.value().source_sha256;
    const auto encoded_journal = encode_journal(journal, payload_buffer_);
    if (!encoded_journal) {
        return core::Result<LegacyImportResult>::failure(encoded_journal.error());
    }
    const auto journal_saved =
        settings_->save(journal_descriptor, std::span<const std::byte>{payload_buffer_.data(),
                                                                       kLegacyImportJournalBytes});
    if (!journal_saved) {
        return core::Result<LegacyImportResult>::failure(journal_saved.error());
    }
    return core::Result<LegacyImportResult>::success(result);
}

core::Status LegacyImportCoordinator::confirm_boot() noexcept {
    if (settings_ == nullptr || payload_buffer_.size() < kLegacyImportJournalBytes ||
        readback_buffer_.size() < kLegacyImportJournalBytes) {
        return core::Status::failure(coordinator_error(core::ErrorCode::invalid_argument,
                                                       "confirm-import", "invalid-buffers"));
    }
    const core::ComponentDescriptor descriptor = descriptor_for(kJournalComponentId);
    const auto loaded = settings_->load(descriptor, readback_buffer_);
    if (!loaded) {
        if (loaded.error().code == core::ErrorCode::not_found) {
            return core::Status::success();
        }
        return core::Status::failure(loaded.error());
    }
    const auto decoded = decode_journal(
        std::span<const std::byte>{readback_buffer_.data(), loaded.value().payload_size});
    if (!decoded) {
        return core::Status::failure(decoded.error());
    }
    if (decoded.value().state == JournalState::confirmed) {
        return core::Status::success();
    }
    Journal confirmed = decoded.value();
    confirmed.state = JournalState::confirmed;
    const auto encoded = encode_journal(confirmed, payload_buffer_);
    if (!encoded) {
        return encoded;
    }
    return settings_->save(
        descriptor, std::span<const std::byte>{payload_buffer_.data(), kLegacyImportJournalBytes});
}

} // namespace blip::storage
