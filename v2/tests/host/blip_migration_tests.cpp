#include "blip/storage/imported_settings.hpp"
#include "blip/storage/legacy_import_coordinator.hpp"
#include "blip/storage/migration_registry.hpp"
#include "test_harness.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string_view>

#ifndef BLIP_V1_SETTINGS_FIXTURE
#error "BLIP_V1_SETTINGS_FIXTURE must name the golden MessagePack fixture"
#endif

namespace {

using blip::core::ErrorCode;
using blip::core::ErrorDomain;
using blip::core::Result;
using blip::core::Status;
using namespace blip::storage;

struct Fixture {
    std::array<std::byte, kMaxLegacySettingsBytes> bytes{};
    std::size_t size{};
};

[[nodiscard]] bool load_fixture(Fixture& fixture) {
    std::FILE* file = std::fopen(BLIP_V1_SETTINGS_FIXTURE, "rb");
    if (file == nullptr) {
        return false;
    }
    fixture.size = std::fread(fixture.bytes.data(), 1, fixture.bytes.size(), file);
    const bool okay = std::feof(file) != 0 && std::fclose(file) == 0 && fixture.size != 0U;
    return okay;
}

[[nodiscard]] Result<std::size_t> append_byte(std::span<const std::byte> input,
                                              std::span<std::byte> output,
                                              std::byte value) noexcept {
    if (output.size() < input.size() + 1U) {
        return Result<std::size_t>::failure(
            {ErrorDomain::storage, ErrorCode::capacity_exceeded, {}, "test-migrate", "output"});
    }
    if (!input.empty()) {
        std::memcpy(output.data(), input.data(), input.size());
    }
    output[input.size()] = value;
    return Result<std::size_t>::success(input.size() + 1U);
}

[[nodiscard]] Result<std::size_t> migrate_one_to_two(std::span<const std::byte> input,
                                                     std::span<std::byte> output) noexcept {
    return append_byte(input, output, std::byte{2});
}

[[nodiscard]] Result<std::size_t> migrate_two_to_three(std::span<const std::byte> input,
                                                       std::span<std::byte> output) noexcept {
    return append_byte(input, output, std::byte{3});
}

bool migration_registry_is_ordered_and_idempotent() {
    std::array<MigrationStep, 2> step_storage{};
    MigrationRegistry registry{step_storage};
    BLIP_CHECK(registry.add({"blip.test", 2, 3, migrate_two_to_three}));
    BLIP_CHECK(registry.add({"blip.test", 1, 2, migrate_one_to_two}));
    auto duplicate = registry.add({"blip.test", 1, 2, migrate_one_to_two});
    BLIP_CHECK(!duplicate && duplicate.error().code == ErrorCode::duplicate_id);
    auto invalid = registry.add({"blip.other", 1, 3, migrate_one_to_two});
    BLIP_CHECK(!invalid && invalid.error().code == ErrorCode::invalid_argument);

    constexpr std::array<std::byte, 1> input{std::byte{1}};
    std::array<std::byte, 8> output{};
    std::array<std::byte, 8> scratch{};
    auto migrated = registry.migrate("blip.test", 1, 3, input, output, scratch);
    BLIP_CHECK(migrated && migrated.value().schema_version == 3 &&
               migrated.value().steps_applied == 2 && migrated.value().payload_size == 3);
    BLIP_CHECK(output[0] == std::byte{1} && output[1] == std::byte{2} && output[2] == std::byte{3});
    std::array<std::byte, 8> repeated{};
    migrated = registry.migrate("blip.test", 1, 3, input, repeated, scratch);
    BLIP_CHECK(migrated && std::memcmp(output.data(), repeated.data(), 3) == 0);
    migrated = registry.migrate("blip.test", 3, 3, input, repeated, scratch);
    BLIP_CHECK(migrated && migrated.value().steps_applied == 0 && repeated[0] == input[0]);
    migrated = registry.migrate("blip.missing", 1, 2, input, output, scratch);
    BLIP_CHECK(!migrated && migrated.error().code == ErrorCode::incompatible_version);
    return true;
}

struct ImportChecks {
    std::size_t count{};
    bool wifi_ssid{};
    bool remote_port{};
    bool motion_array{};
    bool device_name{};
};

[[nodiscard]] Status check_imported_setting(void* context,
                                            const ImportedSetting& setting) noexcept {
    auto& checks = *static_cast<ImportChecks*>(context);
    ++checks.count;
    if (setting.component_id == "blip.transport.wifi" && setting.field == "ssid") {
        checks.wifi_ssid = setting.value.type == LegacyValueType::string &&
                           setting.value.string == "fixture-network";
    } else if (setting.component_id == "blip.osc" && setting.field == "remotePort") {
        checks.remote_port = setting.value.type == LegacyValueType::unsigned_integer &&
                             setting.value.unsigned_integer == 10000U;
    } else if (setting.component_id == "blip.sensor.motion" && setting.field == "accelThresholds") {
        checks.motion_array = setting.value.type == LegacyValueType::numeric_array &&
                              setting.value.numeric_array.size() == 3U &&
                              std::abs(setting.value.numeric_array[0] - 0.8) < 0.000001 &&
                              std::abs(setting.value.numeric_array[1] - 2.0) < 0.000001 &&
                              std::abs(setting.value.numeric_array[2] - 4.0) < 0.000001;
    } else if (setting.component_id == "blip.system.settings" && setting.field == "deviceName") {
        checks.device_name =
            setting.value.type == LegacyValueType::string && setting.value.string == "Fixture BLIP";
    }
    return Status::success();
}

bool golden_messagepack_is_strictly_imported() {
    Fixture fixture{};
    BLIP_CHECK(load_fixture(fixture));
    LegacyImportWorkspace workspace{};
    LegacySettingsImporter importer{};
    ImportChecks checks{};
    const auto imported =
        importer.import(std::span<const std::byte>{fixture.bytes.data(), fixture.size}, workspace,
                        check_imported_setting, &checks);
    BLIP_CHECK(imported && imported.value().setting_count == 112U &&
               imported.value().component_count == 22U && checks.count == 112U);
    constexpr std::array<std::uint8_t, 32> expected_hash{
        0xbd, 0x9b, 0xed, 0x78, 0xd6, 0x10, 0x86, 0x8b, 0x60, 0xef, 0x82,
        0x0e, 0xf5, 0xf4, 0x47, 0x1a, 0xab, 0x02, 0x43, 0x31, 0xeb, 0xcf,
        0x7f, 0xa8, 0x6c, 0x15, 0x4e, 0xed, 0x93, 0x44, 0x19, 0x2e};
    BLIP_CHECK(std::memcmp(imported.value().source_sha256.data(), expected_hash.data(),
                           expected_hash.size()) == 0);
    BLIP_CHECK(checks.wifi_ssid && checks.remote_port && checks.motion_array && checks.device_name);

    for (std::size_t size = 0; size < fixture.size; ++size) {
        const auto truncated =
            importer.import(std::span<const std::byte>{fixture.bytes.data(), size}, workspace);
        BLIP_CHECK(!truncated);
    }
    auto corrupt = fixture;
    corrupt.bytes[0] = std::byte{0xc0};
    const auto rejected =
        importer.import(std::span<const std::byte>{corrupt.bytes.data(), corrupt.size}, workspace);
    BLIP_CHECK(!rejected);
    return true;
}

class FakeLegacySource final : public LegacySettingsSource {
  public:
    explicit FakeLegacySource(std::span<const std::byte> value) noexcept : value_(value) {}

    [[nodiscard]] Result<std::size_t> read(std::span<std::byte> output) noexcept override {
        ++reads;
        if (!present) {
            return Result<std::size_t>::failure(
                {ErrorDomain::storage, ErrorCode::not_found, {}, "source", "missing"});
        }
        if (output.size() < value_.size()) {
            return Result<std::size_t>::failure(
                {ErrorDomain::storage, ErrorCode::capacity_exceeded, {}, "source", "capacity"});
        }
        std::memcpy(output.data(), value_.data(), value_.size());
        return Result<std::size_t>::success(value_.size());
    }

    std::size_t reads{};
    bool present{true};

  private:
    std::span<const std::byte> value_{};
};

class FakeBlobStore final : public BlobStore {
  public:
    enum class FailureMode : std::uint8_t { none, before_mutation, after_mutation };
    static constexpr std::size_t kEntryCount = 64;
    static constexpr std::size_t kValueBytes = 1280;

    struct Entry {
        std::array<char, 16> key{};
        std::array<std::byte, kValueBytes> value{};
        std::size_t size{};
        bool occupied{};
    };

    [[nodiscard]] Result<std::size_t> read(std::string_view key,
                                           std::span<std::byte> output) noexcept override {
        const Entry* entry = find(key);
        if (entry == nullptr) {
            return Result<std::size_t>::failure(
                {ErrorDomain::storage, ErrorCode::not_found, {}, "read", "missing"});
        }
        if (output.size() < entry->size) {
            return Result<std::size_t>::failure(
                {ErrorDomain::storage, ErrorCode::capacity_exceeded, {}, "read", "capacity"});
        }
        std::memcpy(output.data(), entry->value.data(), entry->size);
        return Result<std::size_t>::success(entry->size);
    }

    [[nodiscard]] Status write(std::string_view key,
                               std::span<const std::byte> value) noexcept override {
        ++mutations;
        if (fail_on == mutations && failure_mode == FailureMode::before_mutation) {
            return power_cut();
        }
        Entry* entry = find_or_create(key);
        if (entry == nullptr || value.size() > entry->value.size()) {
            return Status::failure(
                {ErrorDomain::storage, ErrorCode::storage_full, {}, "write", "capacity"});
        }
        std::memcpy(entry->value.data(), value.data(), value.size());
        entry->size = value.size();
        if (fail_on == mutations && failure_mode == FailureMode::after_mutation) {
            return power_cut();
        }
        return Status::success();
    }

    void reset() noexcept {
        for (auto& entry : entries) {
            entry.occupied = false;
            entry.size = 0;
            entry.key[0] = '\0';
        }
        mutations = 0;
        fail_on = 0;
        failure_mode = FailureMode::none;
    }

    void fail(std::size_t mutation, FailureMode mode) noexcept {
        fail_on = mutation;
        failure_mode = mode;
    }

    void clear_failure() noexcept {
        fail_on = 0;
        failure_mode = FailureMode::none;
    }

    std::array<Entry, kEntryCount> entries{};
    std::size_t mutations{};
    std::size_t fail_on{};
    FailureMode failure_mode{FailureMode::none};

  private:
    [[nodiscard]] Entry* find(std::string_view key) noexcept {
        for (auto& entry : entries) {
            if (entry.occupied && std::string_view{entry.key.data()} == key) {
                return &entry;
            }
        }
        return nullptr;
    }

    [[nodiscard]] const Entry* find(std::string_view key) const noexcept {
        for (const auto& entry : entries) {
            if (entry.occupied && std::string_view{entry.key.data()} == key) {
                return &entry;
            }
        }
        return nullptr;
    }

    [[nodiscard]] Entry* find_or_create(std::string_view key) noexcept {
        if (Entry* entry = find(key); entry != nullptr) {
            return entry;
        }
        for (auto& entry : entries) {
            if (!entry.occupied && key.size() < entry.key.size()) {
                std::memcpy(entry.key.data(), key.data(), key.size());
                entry.key[key.size()] = '\0';
                entry.occupied = true;
                return &entry;
            }
        }
        return nullptr;
    }

    [[nodiscard]] static Status power_cut() noexcept {
        return Status::failure(
            {ErrorDomain::storage, ErrorCode::io_failed, {}, "write", "power-cut"});
    }
};

[[nodiscard]] blip::core::ComponentDescriptor descriptor(std::string_view id) noexcept {
    blip::core::ComponentDescriptor value{};
    value.schema_version = 1;
    value.id = id;
    value.display_name = id;
    value.description = "test";
    value.settings = {1, 1};
    return value;
}

struct DecodedWifi {
    bool ssid{};
    bool password{};
};

[[nodiscard]] Status check_wifi(void* context, const ImportedSetting& setting) noexcept {
    auto& wifi = *static_cast<DecodedWifi*>(context);
    if (setting.field == "ssid") {
        wifi.ssid = setting.value.type == LegacyValueType::string &&
                    setting.value.string == "fixture-network";
    } else if (setting.field == "pass") {
        wifi.password = setting.value.type == LegacyValueType::string &&
                        setting.value.string == "not-a-real-secret";
    }
    return Status::success();
}

bool coordinator_is_idempotent_and_preserves_source() {
    Fixture fixture{};
    BLIP_CHECK(load_fixture(fixture));
    FakeLegacySource source{{fixture.bytes.data(), fixture.size}};
    FakeBlobStore backend{};
    std::array<std::byte, 4096> settings_scratch{};
    SettingsStore settings{backend, settings_scratch};
    std::array<std::byte, kMaxLegacySettingsBytes> source_buffer{};
    std::array<std::byte, 1024> payload{};
    std::array<std::byte, 1024> readback{};
    LegacyImportWorkspace workspace{};
    LegacyImportCoordinator coordinator{settings, source,   source_buffer,
                                        payload,  readback, workspace};

    auto imported = coordinator.run();
    BLIP_CHECK(
        imported && imported.value().disposition == LegacyImportDisposition::imported_pending &&
        imported.value().imported_components == 22U && imported.value().setting_count == 112U);
    imported = coordinator.run();
    BLIP_CHECK(imported &&
               imported.value().disposition == LegacyImportDisposition::already_pending &&
               imported.value().preserved_components == 22U);
    BLIP_CHECK(coordinator.confirm_boot());
    imported = coordinator.run();
    BLIP_CHECK(imported &&
               imported.value().disposition == LegacyImportDisposition::already_confirmed);
    BLIP_CHECK(source.reads == 3U);

    const auto loaded = settings.load(descriptor("blip.transport.wifi"), readback);
    BLIP_CHECK(loaded);
    ImportedSettingsDecodeWorkspace decode_workspace{};
    DecodedWifi wifi{};
    ImportedSettingsView view{};
    const auto decoded =
        view.visit(std::span<const std::byte>{readback.data(), loaded.value().payload_size},
                   decode_workspace, check_wifi, &wifi);
    BLIP_CHECK(decoded && decoded.value() == 9U && wifi.ssid && wifi.password);
    return true;
}

bool coordinator_recovers_every_write_boundary() {
    Fixture fixture{};
    BLIP_CHECK(load_fixture(fixture));
    static FakeBlobStore backend{};
    for (const auto mode : {FakeBlobStore::FailureMode::before_mutation,
                            FakeBlobStore::FailureMode::after_mutation}) {
        for (std::size_t cut = 1; cut <= 48U; ++cut) {
            backend.reset();
            FakeLegacySource source{{fixture.bytes.data(), fixture.size}};
            std::array<std::byte, 4096> settings_scratch{};
            SettingsStore settings{backend, settings_scratch};
            std::array<std::byte, kMaxLegacySettingsBytes> source_buffer{};
            std::array<std::byte, 1024> payload{};
            std::array<std::byte, 1024> readback{};
            LegacyImportWorkspace workspace{};
            LegacyImportCoordinator coordinator{settings, source,   source_buffer,
                                                payload,  readback, workspace};
            backend.fail(cut, mode);
            static_cast<void>(coordinator.run());
            backend.clear_failure();
            const auto recovered = coordinator.run();
            BLIP_CHECK(recovered);
            BLIP_CHECK(coordinator.confirm_boot());
            const auto confirmed = coordinator.run();
            BLIP_CHECK(confirmed &&
                       confirmed.value().disposition == LegacyImportDisposition::already_confirmed);
        }
    }
    return true;
}

} // namespace

int main() {
    const TestCase tests[]{
        {"ordered migration registry", migration_registry_is_ordered_and_idempotent},
        {"golden V1 settings import", golden_messagepack_is_strictly_imported},
        {"idempotent import coordinator", coordinator_is_idempotent_and_preserves_source},
        {"import interruption recovery", coordinator_recovers_every_write_boundary},
    };
    return run_tests(tests);
}
