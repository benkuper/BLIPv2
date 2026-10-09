#include "release_identity.hpp"
#include "blip/resources/board_manifest.hpp"
#include "esp_app_desc.h"
#include "sdkconfig.h"

namespace {
constexpr std::uint32_t features() noexcept {
    std::uint32_t output{};
#if defined(BLIP_ENABLE_WASM)
    output |= 1U << 0;
#endif
#if defined(BLIP_ENABLE_BLE)
    output |= 1U << 1;
#endif
#if defined(BLIP_ENABLE_CLASSIC_BT)
    output |= 1U << 2;
#endif
#if defined(BLIP_ENABLE_ESPNOW)
    output |= 1U << 3;
#endif
#if defined(BLIP_ENABLE_FLEET)
    output |= 1U << 4;
#endif
#if defined(BLIP_ENABLE_ARTNET)
    output |= 1U << 5;
#endif
#if defined(BLIP_ENABLE_DDP)
    output |= 1U << 6;
#endif
#if defined(BLIP_ENABLE_E131)
    output |= 1U << 7;
#endif
    return output;
}
constexpr auto board = blip::resources::selected_board_manifest();
#if defined(CONFIG_ESPTOOLPY_FLASHSIZE_8MB)
constexpr std::uint32_t flash_bytes = 8388608;
constexpr std::string_view layout = "ota-8mb-v1";
#elif defined(CONFIG_ESPTOOLPY_FLASHSIZE_4MB)
constexpr std::uint32_t flash_bytes = 4194304;
constexpr std::string_view layout = "ota-4mb-v1";
#else
#error "Release identity requires a declared BLIP 4 MB or 8 MB partition layout"
#endif
constexpr blip::ota::ReleaseIdentity identity{"blip-v2", board.id, board.target, layout,
    BLIP_RELEASE_PROFILE, "stable", flash_bytes, features(), 1, BLIP_RELEASE_SEQUENCE, 0, {}};
}

extern "C" __attribute__((used, section(".rodata_custom_desc"), aligned(4)))
const blip::ota::FirmwareReleaseDescriptor blip_release_image_descriptor =
    blip::ota::make_firmware_release_descriptor(identity);

blip::ota::ReleaseIdentity firmware_release_identity(std::uint32_t web_code) noexcept {
    auto output = identity;
    // Referencing the linked descriptor also keeps it alive under section GC.
    output.firmware_code = blip_release_image_descriptor.code;
    output.firmware_version = esp_app_get_description()->version;
    output.web_code = web_code;
    return output;
}
