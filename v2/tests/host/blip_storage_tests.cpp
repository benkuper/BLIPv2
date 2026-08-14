#include "blip/storage/settings_store.hpp"
#include "test_harness.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

namespace {

using blip::core::ComponentDescriptor;
using blip::core::ErrorCode;
using blip::core::ErrorDomain;
using blip::core::Result;
using blip::core::Status;
using blip::storage::BlobStore;
using blip::storage::LoadedSettings;
using blip::storage::SettingsSlot;
using blip::storage::SettingsStore;

constexpr std::size_t kBlobCapacity = 512;
constexpr std::size_t kEntryCapacity = 12;

class FakeBlobStore final : public BlobStore {
  public:
    enum class FailureMode : std::uint8_t { none, before_write, after_write };

    FakeBlobStore() = default;
    FakeBlobStore(const FakeBlobStore& other) noexcept
        : entries(other.entries), write_log(other.write_log), write_log_size(other.write_log_size),
          write_count(other.write_count), fail_on_write(other.fail_on_write),
          failure_mode(other.failure_mode), force_full(other.force_full) {}

    struct Entry {
        std::array<char, 16> key{};
        std::array<std::byte, kBlobCapacity> value{};
        std::size_t size{};
        bool occupied{};
    };

    [[nodiscard]] Result<std::size_t> read(std::string_view key,
                                           std::span<std::byte> output) noexcept override {
        const Entry* entry = find(key);
        if (entry == nullptr) {
            return Result<std::size_t>::failure(
                {ErrorDomain::storage, ErrorCode::not_found, {}, "read", "missing-key"});
        }
        if (output.size() < entry->size) {
            return Result<std::size_t>::failure({ErrorDomain::storage,
                                                 ErrorCode::capacity_exceeded,
                                                 {},
                                                 "read",
                                                 "output-too-small"});
        }
        std::memcpy(output.data(), entry->value.data(), entry->size);
        return Result<std::size_t>::success(entry->size);
    }

    [[nodiscard]] Status write(std::string_view key,
                               std::span<const std::byte> value) noexcept override {
        ++write_count;
        if (fail_on_write == write_count && failure_mode == FailureMode::before_write) {
            return io_failure();
        }
        if (force_full) {
            return Status::failure(
                {ErrorDomain::storage, ErrorCode::storage_full, {}, "write", "synthetic-full"});
        }
        if (key.empty() || key.size() >= entries[0].key.size() || value.size() > kBlobCapacity) {
            return Status::failure(
                {ErrorDomain::storage, ErrorCode::capacity_exceeded, {}, "write", "fake-capacity"});
        }

        Entry* entry = find(key);
        if (entry == nullptr) {
            for (auto& candidate : entries) {
                if (!candidate.occupied) {
                    entry = &candidate;
                    break;
                }
            }
        }
        if (entry == nullptr) {
            return Status::failure(
                {ErrorDomain::storage, ErrorCode::storage_full, {}, "write", "entry-capacity"});
        }

        entry->key.fill('\0');
        std::memcpy(entry->key.data(), key.data(), key.size());
        std::memcpy(entry->value.data(), value.data(), value.size());
        entry->size = value.size();
        entry->occupied = true;
        if (write_log_size < write_log.size()) {
            write_log[write_log_size++] = entry->key;
        }

        if (fail_on_write == write_count && failure_mode == FailureMode::after_write) {
            return io_failure();
        }
        return Status::success();
    }

    void fail_write(std::size_t call, FailureMode mode) noexcept {
        fail_on_write = call;
        failure_mode = mode;
    }

    void clear_failure() noexcept {
        fail_on_write = 0;
        failure_mode = FailureMode::none;
        force_full = false;
    }

    [[nodiscard]] bool corrupt(std::string_view key, std::size_t offset) noexcept {
        Entry* entry = find(key);
        if (entry == nullptr || offset >= entry->size) {
            return false;
        }
        entry->value[offset] ^= std::byte{0x5a};
        return true;
    }

    [[nodiscard]] std::string_view written_key(std::size_t index) const noexcept {
        if (index >= write_log_size) {
            return {};
        }
        return std::string_view{write_log[index].data()};
    }

    [[nodiscard]] std::span<const std::byte> value(std::string_view key) const noexcept {
        const Entry* entry = find(key);
        return entry == nullptr ? std::span<const std::byte>{}
                                : std::span<const std::byte>{entry->value.data(), entry->size};
    }

    std::array<Entry, kEntryCapacity> entries{};
    std::array<std::array<char, 16>, 32> write_log{};
    std::size_t write_log_size{};
    std::size_t write_count{};
    std::size_t fail_on_write{};
    FailureMode failure_mode{FailureMode::none};
    bool force_full{};

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

    [[nodiscard]] static Status io_failure() noexcept {
        return Status::failure(
            {ErrorDomain::storage, ErrorCode::io_failed, {}, "write", "power-cut"});
    }
};

[[nodiscard]] constexpr ComponentDescriptor descriptor(std::string_view id,
                                                       std::uint32_t schema) noexcept {
    ComponentDescriptor value{};
    value.schema_version = 1;
    value.id = id;
    value.display_name = id;
    value.description = "settings test component";
    value.settings = {schema, schema};
    return value;
}

template <std::size_t Count>
[[nodiscard]] constexpr std::span<const std::byte>
bytes(const std::array<std::uint8_t, Count>& value) noexcept {
    return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
}

template <std::size_t Count>
[[nodiscard]] bool equals(std::span<const std::byte> actual,
                          const std::array<std::uint8_t, Count>& expected) noexcept {
    return actual.size() == expected.size() &&
           std::memcmp(actual.data(), expected.data(), expected.size()) == 0;
}

bool empty_store_reports_not_found() {
    FakeBlobStore backend{};
    std::array<std::byte, kBlobCapacity> scratch{};
    std::array<std::byte, 32> output{};
    SettingsStore store{backend, scratch};

    const auto loaded = store.load(descriptor("test.empty", 1), output);
    BLIP_CHECK(!loaded);
    BLIP_CHECK(loaded.error().domain == ErrorDomain::storage);
    BLIP_CHECK(loaded.error().code == ErrorCode::not_found);
    return true;
}

bool versioned_round_trip_and_slot_rotation() {
    FakeBlobStore backend{};
    std::array<std::byte, kBlobCapacity> scratch{};
    std::array<std::byte, 32> output{};
    SettingsStore store{backend, scratch};
    constexpr std::array<std::uint8_t, 5> first{0x81, 0xa1, 0x78, 0x01, 0xee};
    constexpr std::array<std::uint8_t, 4> second{0x82, 0x10, 0x20, 0xf0};

    BLIP_CHECK(store.save(descriptor("test.versioned", 7), bytes(first)));
    auto loaded = store.load(descriptor("test.versioned", 7), output);
    BLIP_CHECK(loaded);
    BLIP_CHECK(loaded.value().schema_version == 7);
    BLIP_CHECK(loaded.value().generation == 1);
    BLIP_CHECK(loaded.value().slot == SettingsSlot::a);
    BLIP_CHECK(
        equals(std::span<const std::byte>{output.data(), loaded.value().payload_size}, first));

    BLIP_CHECK(store.save(descriptor("test.versioned", 8), bytes(second)));
    loaded = store.load(descriptor("test.versioned", 8), output);
    BLIP_CHECK(loaded);
    BLIP_CHECK(loaded.value().schema_version == 8);
    BLIP_CHECK(loaded.value().generation == 2);
    BLIP_CHECK(loaded.value().slot == SettingsSlot::b);
    BLIP_CHECK(
        equals(std::span<const std::byte>{output.data(), loaded.value().payload_size}, second));
    return true;
}

bool persisted_format_is_stable() {
    constexpr auto component = descriptor("test.golden", 7);
    constexpr std::array<std::uint8_t, 2> payload{0xde, 0xad};
    constexpr std::array<std::uint8_t, 45> expected_slot{
        0x42, 0x4c, 0x50, 0x53, 0x01, 0x00, 0x20, 0x00, 0x07, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x0b, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0xd4, 0x83,
        0xa8, 0x38, 0x74, 0x65, 0x73, 0x74, 0x2e, 0x67, 0x6f, 0x6c, 0x64, 0x65, 0x6e, 0xde, 0xad};
    constexpr std::array<std::uint8_t, 32> expected_commit{
        0x42, 0x4c, 0x50, 0x43, 0x01, 0x00, 0x20, 0x00, 0x20, 0x99, 0x36,
        0x16, 0xec, 0x6b, 0xe8, 0x66, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x42, 0xe7, 0xac, 0xd1};
    FakeBlobStore backend{};
    std::array<std::byte, kBlobCapacity> scratch{};
    SettingsStore store{backend, scratch};

    BLIP_CHECK(store.save(component, bytes(payload)));
    BLIP_CHECK(backend.written_key(0) == "a6bec16369920");
    BLIP_CHECK(backend.written_key(1) == "c6bec16369920");
    BLIP_CHECK(equals(backend.value(backend.written_key(0)), expected_slot));
    BLIP_CHECK(equals(backend.value(backend.written_key(1)), expected_commit));
    return true;
}

bool interrupted_save_returns_previous_or_new() {
    constexpr auto component = descriptor("test.atomic", 1);
    constexpr std::array<std::uint8_t, 3> old_value{1, 2, 3};
    constexpr std::array<std::uint8_t, 4> new_value{7, 8, 9, 10};
    FakeBlobStore baseline{};
    std::array<std::byte, kBlobCapacity> baseline_scratch{};
    SettingsStore baseline_store{baseline, baseline_scratch};
    BLIP_CHECK(baseline_store.save(component, bytes(old_value)));
    BLIP_CHECK(baseline.write_count == 2);

    struct Scenario {
        std::size_t write_call;
        FakeBlobStore::FailureMode mode;
        bool expect_new;
    };
    const Scenario scenarios[]{
        {3, FakeBlobStore::FailureMode::before_write, false},
        {3, FakeBlobStore::FailureMode::after_write, false},
        {4, FakeBlobStore::FailureMode::before_write, false},
        {4, FakeBlobStore::FailureMode::after_write, true},
    };

    for (const auto& scenario : scenarios) {
        auto interrupted = baseline;
        interrupted.fail_write(scenario.write_call, scenario.mode);
        std::array<std::byte, kBlobCapacity> scratch{};
        SettingsStore store{interrupted, scratch};
        BLIP_CHECK(!store.save(component, bytes(new_value)));

        interrupted.clear_failure();
        std::array<std::byte, 32> output{};
        const auto loaded = store.load(component, output);
        BLIP_CHECK(loaded);
        const auto payload = std::span<const std::byte>{output.data(), loaded.value().payload_size};
        BLIP_CHECK(scenario.expect_new ? equals(payload, new_value) : equals(payload, old_value));
    }
    return true;
}

bool corrupt_selected_slot_falls_back() {
    constexpr auto component = descriptor("test.fallback", 1);
    constexpr std::array<std::uint8_t, 2> old_value{0x11, 0x22};
    constexpr std::array<std::uint8_t, 2> new_value{0x33, 0x44};
    FakeBlobStore backend{};
    std::array<std::byte, kBlobCapacity> scratch{};
    SettingsStore store{backend, scratch};
    BLIP_CHECK(store.save(component, bytes(old_value)));
    BLIP_CHECK(store.save(component, bytes(new_value)));
    BLIP_CHECK(backend.corrupt(backend.written_key(2), 0));

    std::array<std::byte, 16> output{};
    const auto loaded = store.load(component, output);
    BLIP_CHECK(loaded);
    BLIP_CHECK(loaded.value().generation == 1);
    BLIP_CHECK(
        equals(std::span<const std::byte>{output.data(), loaded.value().payload_size}, old_value));
    return true;
}

bool torn_commit_uses_highest_valid_generation() {
    constexpr auto component = descriptor("test.commit", 1);
    constexpr std::array<std::uint8_t, 1> old_value{0x01};
    constexpr std::array<std::uint8_t, 1> new_value{0x02};
    FakeBlobStore backend{};
    std::array<std::byte, kBlobCapacity> scratch{};
    SettingsStore store{backend, scratch};
    BLIP_CHECK(store.save(component, bytes(old_value)));
    BLIP_CHECK(store.save(component, bytes(new_value)));
    BLIP_CHECK(backend.corrupt(backend.written_key(3), 4));

    std::array<std::byte, 8> output{};
    const auto loaded = store.load(component, output);
    BLIP_CHECK(loaded);
    BLIP_CHECK(loaded.value().generation == 2);
    BLIP_CHECK(
        equals(std::span<const std::byte>{output.data(), loaded.value().payload_size}, new_value));
    return true;
}

bool corrupt_only_record_is_diagnosed() {
    constexpr auto component = descriptor("test.corrupt", 1);
    constexpr std::array<std::uint8_t, 2> value{0xaa, 0xbb};
    FakeBlobStore backend{};
    std::array<std::byte, kBlobCapacity> scratch{};
    SettingsStore store{backend, scratch};
    BLIP_CHECK(store.save(component, bytes(value)));
    BLIP_CHECK(backend.corrupt(backend.written_key(0), 31));

    std::array<std::byte, 8> output{};
    const auto loaded = store.load(component, output);
    BLIP_CHECK(!loaded);
    BLIP_CHECK(loaded.error().code == ErrorCode::corrupt_data);
    return true;
}

bool components_are_isolated() {
    constexpr auto alpha = descriptor("test.alpha", 1);
    constexpr auto beta = descriptor("test.beta", 3);
    constexpr std::array<std::uint8_t, 2> alpha_value{1, 1};
    constexpr std::array<std::uint8_t, 3> beta_value{2, 2, 2};
    FakeBlobStore backend{};
    std::array<std::byte, kBlobCapacity> scratch{};
    SettingsStore store{backend, scratch};
    BLIP_CHECK(store.save(alpha, bytes(alpha_value)));
    BLIP_CHECK(store.save(beta, bytes(beta_value)));

    std::array<std::byte, 8> output{};
    auto loaded = store.load(alpha, output);
    BLIP_CHECK(loaded && loaded.value().generation == 1);
    BLIP_CHECK(equals(std::span<const std::byte>{output.data(), loaded.value().payload_size},
                      alpha_value));
    loaded = store.load(beta, output);
    BLIP_CHECK(loaded && loaded.value().schema_version == 3);
    BLIP_CHECK(
        equals(std::span<const std::byte>{output.data(), loaded.value().payload_size}, beta_value));
    return true;
}

bool bounded_failures_are_explicit() {
    constexpr auto component = descriptor("test.bounds", 1);
    constexpr std::array<std::uint8_t, 16> value{};
    FakeBlobStore backend{};
    std::array<std::byte, 48> small_scratch{};
    SettingsStore small_store{backend, small_scratch};
    auto status = small_store.save(component, bytes(value));
    BLIP_CHECK(!status);
    BLIP_CHECK(status.error().code == ErrorCode::capacity_exceeded);

    std::array<std::byte, kBlobCapacity> scratch{};
    SettingsStore store{backend, scratch};
    backend.force_full = true;
    status = store.save(component, bytes(value));
    BLIP_CHECK(!status);
    BLIP_CHECK(status.error().code == ErrorCode::storage_full);
    return true;
}

} // namespace

int main() {
    const TestCase tests[]{
        {"empty settings store", empty_store_reports_not_found},
        {"versioned round trip and slot rotation", versioned_round_trip_and_slot_rotation},
        {"stable persisted format", persisted_format_is_stable},
        {"interrupted save atomicity", interrupted_save_returns_previous_or_new},
        {"corrupt selected slot fallback", corrupt_selected_slot_falls_back},
        {"torn commit fallback", torn_commit_uses_highest_valid_generation},
        {"corrupt record diagnosis", corrupt_only_record_is_diagnosed},
        {"per-component isolation", components_are_isolated},
        {"bounded storage failures", bounded_failures_are_explicit},
    };
    return run_tests(tests);
}
