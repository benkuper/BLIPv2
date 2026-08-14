#include "blip/storage/legacy_settings_importer.hpp"

#include <array>
#include <cinttypes>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace {

using namespace blip::storage;

struct DumpContext {
    bool first{true};
};

void print_json_string(std::string_view value) {
    std::putchar('"');
    for (const unsigned char character : value) {
        switch (character) {
        case '"':
            std::fputs("\\\"", stdout);
            break;
        case '\\':
            std::fputs("\\\\", stdout);
            break;
        case '\b':
            std::fputs("\\b", stdout);
            break;
        case '\f':
            std::fputs("\\f", stdout);
            break;
        case '\n':
            std::fputs("\\n", stdout);
            break;
        case '\r':
            std::fputs("\\r", stdout);
            break;
        case '\t':
            std::fputs("\\t", stdout);
            break;
        default:
            if (character < 0x20U) {
                std::printf("\\u%04x", static_cast<unsigned>(character));
            } else {
                std::putchar(character);
            }
            break;
        }
    }
    std::putchar('"');
}

[[nodiscard]] blip::core::Status dump_setting(void* context,
                                              const ImportedSetting& setting) noexcept {
    auto& dump = *static_cast<DumpContext*>(context);
    if (!dump.first) {
        std::putchar(',');
    }
    dump.first = false;
    std::fputs("{\"legacy_component_path\":", stdout);
    print_json_string(setting.legacy_component_path);
    std::fputs(",\"component_id\":", stdout);
    print_json_string(setting.component_id);
    std::fputs(",\"field\":", stdout);
    print_json_string(setting.field);
    std::fputs(",\"value\":", stdout);
    switch (setting.value.type) {
    case LegacyValueType::boolean:
        std::fputs(setting.value.boolean ? "true" : "false", stdout);
        break;
    case LegacyValueType::signed_integer:
        std::printf("%" PRId64, setting.value.signed_integer);
        break;
    case LegacyValueType::unsigned_integer:
        std::printf("%" PRIu64, setting.value.unsigned_integer);
        break;
    case LegacyValueType::floating:
        std::printf("%.17g", setting.value.floating);
        break;
    case LegacyValueType::string:
        print_json_string(setting.value.string);
        break;
    case LegacyValueType::numeric_array:
        std::putchar('[');
        for (std::size_t index = 0; index < setting.value.numeric_array.size(); ++index) {
            if (index != 0U) {
                std::putchar(',');
            }
            std::printf("%.17g", setting.value.numeric_array[index]);
        }
        std::putchar(']');
        break;
    }
    std::putchar('}');
    return blip::core::Status::success();
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fputs("usage: blip_legacy_import_dump <settings.msgpack>\n", stderr);
        return 2;
    }
    std::FILE* file = std::fopen(argv[1], "rb");
    if (file == nullptr) {
        return 2;
    }
    std::array<std::byte, kMaxLegacySettingsBytes> source{};
    const std::size_t size = std::fread(source.data(), 1, source.size(), file);
    if (std::feof(file) == 0 || std::fclose(file) != 0 || size == 0U) {
        return 2;
    }
    LegacyImportWorkspace workspace{};
    LegacySettingsImporter importer{};
    DumpContext context{};
    std::fputs("{\"values\":[", stdout);
    const auto imported = importer.import({source.data(), size}, workspace, dump_setting, &context);
    if (!imported) {
        std::fprintf(stderr, "import failed: %.*s\n",
                     static_cast<int>(imported.error().detail.size()),
                     imported.error().detail.data());
        return 1;
    }
    std::fputs("],\"setting_count\":", stdout);
    std::printf("%zu", imported.value().setting_count);
    std::fputs(",\"component_count\":", stdout);
    std::printf("%zu", imported.value().component_count);
    std::fputs(",\"source_sha256\":\"", stdout);
    for (const std::byte value : imported.value().source_sha256) {
        std::printf("%02x", std::to_integer<unsigned>(value));
    }
    std::fputs("\"}\n", stdout);
    return 0;
}
