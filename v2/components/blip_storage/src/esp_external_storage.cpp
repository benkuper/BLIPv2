#include "blip/storage/esp_external_storage.hpp"

#if defined(BLIP_HAS_EXTERNAL_STORAGE)
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/sdspi_host.h"
#include "soc/soc_caps.h"
#if defined(BLIP_EXTERNAL_MMC) && SOC_SDMMC_HOST_SUPPORTED
#include "driver/sdmmc_host.h"
#endif
#include "diskio_impl.h"
#include "sdmmc_cmd.h"
#include "esp_flash_spi_init.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <algorithm>
#include <cerrno>
#include <sys/stat.h>

namespace blip::storage {
namespace {
constexpr char kMount[] = "/blipmedia";
constexpr char kNamespace[] = "/blipmedia/blip-v2";
EspExternalStorage* software_volume{};
BYTE software_drive{0xff};

DSTATUS disk_status(BYTE drive) {
    return software_volume && drive == software_drive && software_volume->software_card().sectors()
               ? 0 : STA_NOINIT;
}
DRESULT disk_read(BYTE drive, BYTE* bytes, DWORD sector, UINT count) {
    if (disk_status(drive) || !bytes || !count) return RES_PARERR;
    std::lock_guard guard(software_volume->disk_mutex());
    if (software_volume->software_card().read(sector,
        {reinterpret_cast<std::byte*>(bytes), std::size_t(count) * 512U})) return RES_OK;
    ESP_LOGW("blip.media", "SD read failed: %s", software_volume->software_card().error());
    return RES_ERROR;
}
DRESULT disk_write(BYTE drive, const BYTE* bytes, DWORD sector, UINT count) {
    if (disk_status(drive) || !bytes || !count) return RES_PARERR;
    std::lock_guard guard(software_volume->disk_mutex());
    if (software_volume->software_card().write(sector,
        {reinterpret_cast<const std::byte*>(bytes), std::size_t(count) * 512U})) return RES_OK;
    ESP_LOGW("blip.media", "SD write failed: %s", software_volume->software_card().error());
    return RES_ERROR;
}
DRESULT disk_ioctl(BYTE drive, BYTE command, void* output) {
    if (disk_status(drive)) return RES_NOTRDY;
    if (command == CTRL_SYNC) return RES_OK; // Each sector write waits for ready/status.
    if (!output) return RES_PARERR;
    switch (command) {
    case GET_SECTOR_COUNT: *static_cast<DWORD*>(output) = software_volume->software_card().sectors(); return RES_OK;
    case GET_SECTOR_SIZE: *static_cast<WORD*>(output) = 512; return RES_OK;
    case GET_BLOCK_SIZE: *static_cast<DWORD*>(output) = 1; return RES_OK;
    default: return RES_PARERR;
    }
}
constexpr ff_diskio_impl_t kSoftwareDisk{disk_status, disk_status, disk_read, disk_write, disk_ioctl};

#if defined(BLIP_EXTERNAL_NOR)
bool blank(const esp_partition_t& partition) noexcept {
    std::array<std::byte, 512> bytes{};
    for (std::size_t offset = 0; offset < partition.size; offset += bytes.size()) {
        const auto count = std::min(bytes.size(), std::size_t(partition.size) - offset);
        if (esp_partition_read(&partition, offset, bytes.data(), count) != ESP_OK ||
            !std::all_of(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(count),
                         [](std::byte value) { return value == std::byte{0xff}; })) return false;
        if (offset % 16384 == 0) vTaskDelay(1);
    }
    return true;
}
#endif
} // namespace

bool GpioSdSpiBus::initialize(resources::BoardStorage configuration) noexcept {
    if (!GPIO_IS_VALID_OUTPUT_GPIO(configuration.mosi) || !GPIO_IS_VALID_GPIO(configuration.miso) ||
        !GPIO_IS_VALID_OUTPUT_GPIO(configuration.clock) || !GPIO_IS_VALID_OUTPUT_GPIO(configuration.select))
        return false;
    configuration_ = configuration;
    for (const auto pin : {configuration.mosi, configuration.clock, configuration.select}) {
        const auto gpio = static_cast<gpio_num_t>(pin);
        if (gpio_set_level(gpio, pin == configuration.clock ? 0 : 1) != ESP_OK ||
            gpio_set_direction(gpio, GPIO_MODE_OUTPUT) != ESP_OK) return false;
    }
    return gpio_set_direction(static_cast<gpio_num_t>(configuration.miso), GPIO_MODE_INPUT) == ESP_OK &&
           gpio_set_pull_mode(static_cast<gpio_num_t>(configuration.miso), GPIO_PULLUP_ONLY) == ESP_OK;
}
std::uint8_t GpioSdSpiBus::exchange(std::uint8_t value) noexcept {
    std::uint8_t input{};
    for (unsigned bit = 0; bit < 8; ++bit) {
        static_cast<void>(gpio_set_level(static_cast<gpio_num_t>(configuration_.mosi), (value & 0x80U) != 0));
        if (slow_) esp_rom_delay_us(1);
        static_cast<void>(gpio_set_level(static_cast<gpio_num_t>(configuration_.clock), 1));
        if (slow_) esp_rom_delay_us(1);
        input = std::uint8_t((input << 1U) | gpio_get_level(static_cast<gpio_num_t>(configuration_.miso)));
        static_cast<void>(gpio_set_level(static_cast<gpio_num_t>(configuration_.clock), 0));
        value <<= 1U;
    }
    return input;
}
void GpioSdSpiBus::select(bool active) noexcept {
    static_cast<void>(gpio_set_level(static_cast<gpio_num_t>(configuration_.select), active ? 0 : 1));
}
std::uint64_t GpioSdSpiBus::milliseconds() noexcept { return std::uint64_t(esp_timer_get_time() / 1000); }
void GpioSdSpiBus::yield() noexcept { vTaskDelay(1); }

const char* EspExternalStorage::media() const noexcept {
    switch (configuration_.kind) {
    case resources::StorageMediaKind::sd_spi: return "sd-spi";
    case resources::StorageMediaKind::sd_mmc: return "sd-mmc";
    case resources::StorageMediaKind::spi_nor: return "spi-nor";
    default: return "none";
    }
}

bool EspExternalStorage::mount_software_sd() noexcept {
    if (software_volume || !software_bus_.initialize(configuration_) || !software_card_.initialize()) {
        state_ = software_card_.error(); return false;
    }
    if (ff_diskio_get_drive(&drive_number_) != ESP_OK || drive_number_ > 9) {
        state_ = "no-fat-drive"; return false;
    }
    software_drive = drive_number_;
    software_volume = this;
    ff_diskio_register(drive_number_, &kSoftwareDisk);
    drive_[0] = char('0' + drive_number_);
    drive_[1] = ':';
    const esp_vfs_fat_conf_t configuration{kMount, drive_.data(), 4};
    FATFS* filesystem{};
    const auto registered = esp_vfs_fat_register(&configuration, &filesystem);
    fat_ = filesystem;
    if (registered != ESP_OK || f_mount(filesystem, drive_.data(), 1) != FR_OK) {
        state_ = "external-fat-mount-failed"; return false;
    }
    return true;
}

bool EspExternalStorage::mount_spi() noexcept {
    if (configuration_.host == 2) host_ = SPI2_HOST;
#if SOC_SPI_PERIPH_NUM > 2
    else if (configuration_.host == 3) host_ = SPI3_HOST;
#endif
    else { state_ = "unsupported-spi-host"; return false; }
    spi_bus_config_t bus{};
    bus.mosi_io_num = configuration_.mosi;
    bus.miso_io_num = configuration_.miso;
    bus.sclk_io_num = configuration_.clock;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = 4096;
    const auto host = static_cast<spi_host_device_t>(host_);
    if (spi_bus_initialize(host, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
        host_ = -1; state_ = "spi-host-unavailable"; return false;
    }
    if (configuration_.kind == resources::StorageMediaKind::spi_nor) {
#if defined(BLIP_EXTERNAL_NOR)
        esp_flash_spi_device_config_t device{};
        device.host_id = host;
        device.cs_io_num = configuration_.select;
        device.io_mode = SPI_FLASH_SLOWRD;
        device.freq_mhz = 20;
        device.clock_source = SPI_CLK_SRC_DEFAULT;
        std::uint32_t size{};
        if (spi_bus_add_flash_device(&flash_, &device) != ESP_OK ||
            esp_flash_init(flash_) != ESP_OK || esp_flash_get_size(flash_, &size) != ESP_OK ||
            esp_partition_register_external(flash_, 0, size, "blip_external", ESP_PARTITION_TYPE_DATA,
                ESP_PARTITION_SUBTYPE_DATA_LITTLEFS, &partition_) != ESP_OK) {
            state_ = "external-nor-probe-failed"; return false;
        }
        esp_vfs_littlefs_conf_t filesystem{};
        filesystem.base_path = kMount;
        filesystem.partition = partition_;
        auto result = esp_vfs_littlefs_register(&filesystem);
        if (result != ESP_OK && blank(*partition_)) {
            static_cast<void>(esp_vfs_littlefs_unregister_partition(partition_));
            result = esp_littlefs_format_partition(partition_);
            if (result == ESP_OK) result = esp_vfs_littlefs_register(&filesystem);
        }
        if (result != ESP_OK) { state_ = "external-nor-mount-failed"; return false; }
        return true;
#else
        state_ = "nor-provider-not-built"; return false;
#endif
    }
    sdmmc_host_t sd_host = SDSPI_HOST_DEFAULT();
    sd_host.slot = host_;
    sd_host.max_freq_khz = 12000;
    sdspi_device_config_t device = SDSPI_DEVICE_CONFIG_DEFAULT();
    device.host_id = host;
    device.gpio_cs = static_cast<gpio_num_t>(configuration_.select);
    esp_vfs_fat_mount_config_t filesystem{};
    filesystem.max_files = 4;
    sdmmc_card_t* card{};
    const auto result = esp_vfs_fat_sdspi_mount(kMount, &sd_host, &device, &filesystem, &card);
    card_ = card;
    if (result != ESP_OK) {
        state_ = "external-sd-mount-failed"; return false;
    }
    return true;
}

bool EspExternalStorage::mount_mmc() noexcept {
#if defined(BLIP_EXTERNAL_MMC) && SOC_SDMMC_HOST_SUPPORTED
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags = SDMMC_HOST_FLAG_1BIT;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
#if SOC_SDMMC_USE_GPIO_MATRIX
    slot.clk = static_cast<gpio_num_t>(configuration_.clock);
    slot.cmd = static_cast<gpio_num_t>(configuration_.mosi);
    slot.d0 = static_cast<gpio_num_t>(configuration_.miso);
#else
    if (configuration_.clock != 14 || configuration_.mosi != 15 || configuration_.miso != 2) {
        state_ = "unsupported-mmc-pins"; return false;
    }
#endif
    esp_vfs_fat_mount_config_t filesystem{};
    filesystem.max_files = 4;
    sdmmc_card_t* card{};
    const auto result = esp_vfs_fat_sdmmc_mount(kMount, &host, &slot, &filesystem, &card);
    card_ = card;
    if (result == ESP_OK) return true;
#endif
    state_ = "external-mmc-mount-failed"; return false;
}

bool EspExternalStorage::mount() noexcept {
    if (mounted_) return true;
    if (configuration_.kind == resources::StorageMediaKind::none) {
        state_ = "not-fitted"; return false;
    }
    if (configuration_.power >= 0) {
        const auto pin = static_cast<gpio_num_t>(configuration_.power);
        if (!GPIO_IS_VALID_OUTPUT_GPIO(pin) || gpio_set_level(pin, configuration_.power_level) != ESP_OK ||
            gpio_set_direction(pin, GPIO_MODE_OUTPUT) != ESP_OK) {
            state_ = "external-power-pin"; return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    bool result{};
    if (configuration_.kind == resources::StorageMediaKind::sd_mmc) result = mount_mmc();
    else if (configuration_.kind == resources::StorageMediaKind::sd_spi && configuration_.host == 0)
        result = mount_software_sd();
    else result = mount_spi();
    if (result && ::mkdir(kNamespace, 0755) != 0 && errno != EEXIST) {
        state_ = "external-namespace-failed"; result = false;
    }
    if (!result) { static_cast<void>(unmount()); return false; }
    mounted_ = true;
    state_ = "mounted";
    return true;
}

bool EspExternalStorage::unmount() noexcept {
    bool success = true;
    if (fat_) {
        success = f_mount(nullptr, drive_.data(), 0) == FR_OK;
        success = esp_vfs_fat_unregister_path(kMount) == ESP_OK && success;
        fat_ = nullptr;
    }
    if (software_volume == this) {
        ff_diskio_unregister(drive_number_);
        software_volume = nullptr;
        software_drive = 0xff;
    }
    drive_number_ = 0xff;
    if (card_) {
        success = esp_vfs_fat_sdcard_unmount(kMount, static_cast<sdmmc_card_t*>(card_)) == ESP_OK && success;
        card_ = nullptr;
    }
#if defined(BLIP_EXTERNAL_NOR)
    if (partition_) {
        // A failed mount can already have freed the LittleFS instance.
        const auto result = esp_vfs_littlefs_unregister_partition(partition_);
        if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) success = false;
        success = esp_partition_deregister_external(partition_) == ESP_OK && success;
        partition_ = nullptr;
    }
    if (flash_) {
        success = spi_bus_remove_flash_device(flash_) == ESP_OK && success;
        flash_ = nullptr;
    }
#endif
    if (host_ >= 0) {
        success = spi_bus_free(static_cast<spi_host_device_t>(host_)) == ESP_OK && success;
        host_ = -1;
    }
    mounted_ = false;
    return success;
}
} // namespace blip::storage
#else
namespace blip::storage {
bool GpioSdSpiBus::initialize(resources::BoardStorage) noexcept { return false; }
std::uint8_t GpioSdSpiBus::exchange(std::uint8_t) noexcept { return 0xff; }
void GpioSdSpiBus::select(bool) noexcept {}
std::uint64_t GpioSdSpiBus::milliseconds() noexcept { return 0; }
void GpioSdSpiBus::yield() noexcept {}
bool EspExternalStorage::mount() noexcept { state_ = "not-fitted"; return false; }
bool EspExternalStorage::unmount() noexcept { return true; }
const char* EspExternalStorage::media() const noexcept { return "none"; }
} // namespace blip::storage
#endif
