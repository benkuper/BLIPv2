#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace blip::core {

enum class ValueType : std::uint8_t { boolean, integer, number, string };
enum class Access : std::uint8_t { read_only, read_write };
enum class DisablePolicy : std::uint8_t { live, reboot_required };

struct ScalarValue {
    ValueType type{ValueType::integer};
    bool boolean{};
    std::int64_t integer{};
    double number{};
    std::string_view string{};

    [[nodiscard]] static constexpr ScalarValue from_bool(bool value) noexcept {
        ScalarValue result{};
        result.type = ValueType::boolean;
        result.boolean = value;
        return result;
    }

    [[nodiscard]] static constexpr ScalarValue from_integer(std::int64_t value) noexcept {
        ScalarValue result{};
        result.type = ValueType::integer;
        result.integer = value;
        return result;
    }

    [[nodiscard]] static constexpr ScalarValue from_number(double value) noexcept {
        ScalarValue result{};
        result.type = ValueType::number;
        result.number = value;
        return result;
    }

    [[nodiscard]] static constexpr ScalarValue from_string(std::string_view value) noexcept {
        ScalarValue result{};
        result.type = ValueType::string;
        result.string = value;
        return result;
    }
};

struct NumericBounds {
    bool present{};
    double minimum{};
    double maximum{};
    double step{};
};

struct FieldDescriptor {
    std::string_view id{};
    ValueType type{ValueType::integer};
    bool required{};
};

struct ParameterDescriptor {
    std::string_view id{};
    std::string_view label{};
    ValueType type{ValueType::integer};
    Access access{Access::read_write};
    bool persisted{};
    ScalarValue default_value{};
    NumericBounds bounds{};
    std::string_view unit{};
};

struct ActionDescriptor {
    std::string_view id{};
    std::string_view label{};
    std::span<const FieldDescriptor> arguments{};
};

struct EventDescriptor {
    std::string_view id{};
    std::span<const FieldDescriptor> fields{};
};

struct MetadataEntry {
    std::string_view key{};
    std::string_view value{};
};

struct DiagnosticDescriptor {
    std::string_view id{};
    ValueType type{ValueType::integer};
    std::string_view unit{};
};

enum class ResourceClass : std::uint8_t {
    gpio,
    rmt,
    spi,
    i2c,
    uart,
    timer,
    dma,
    internal_memory,
    psram,
    radio,
};

enum class OwnershipMode : std::uint8_t { exclusive, shared_read, bus_member, multiplexed };

struct ResourceRequest {
    ResourceClass resource_class{ResourceClass::gpio};
    std::string_view logical_name{};
    OwnershipMode ownership{OwnershipMode::exclusive};
    std::span<const std::string_view> alternatives{};
    std::uint32_t required_capabilities{};
    std::uint32_t amount{1};
    std::uint32_t member_key{};
    std::uint32_t feature_mask{};
    std::uint32_t incompatible_features{};
    bool live_reacquire{};
};

struct SettingsDescriptor {
    std::uint32_t schema_version{};
    std::uint32_t migration_version{};
};

struct CostDescriptor {
    std::uint32_t flash_bytes{};
    std::uint32_t static_ram_bytes{};
    std::uint32_t task_stack_bytes{};
};

struct ComponentDescriptor {
    std::uint32_t schema_version{};
    std::string_view id{};
    std::string_view display_name{};
    std::string_view description{};
    std::span<const MetadataEntry> metadata{};
    std::span<const std::string_view> provided_services{};
    std::span<const std::string_view> required_services{};
    std::span<const std::string_view> optional_services{};
    std::span<const ParameterDescriptor> parameters{};
    std::span<const ActionDescriptor> actions{};
    std::span<const EventDescriptor> events{};
    std::span<const DiagnosticDescriptor> diagnostics{};
    std::span<const ResourceRequest> resources{};
    SettingsDescriptor settings{};
    DisablePolicy disable_policy{DisablePolicy::reboot_required};
    bool supports_resume{};
    bool supports_restart{};
    CostDescriptor cost{};
};

enum class DynamicControlKind : std::uint8_t { parameter, action, event };

struct DynamicControl {
    std::string_view component_id{};
    DynamicControlKind kind{DynamicControlKind::parameter};
    std::string_view id{};
    ValueType value_type{ValueType::integer};
};

} // namespace blip::core
