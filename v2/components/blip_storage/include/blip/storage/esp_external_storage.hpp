#pragma once

#include "blip/resources/board_manifest.hpp"
#include "blip/storage/sd_spi_card.hpp"
#include "esp_flash.h"
#include "esp_partition.h"

#include <mutex>

namespace blip::storage {
class GpioSdSpiBus final : public SdSpiBus {
  public:
    [[nodiscard]] bool initialize(resources::BoardStorage configuration) noexcept;
    std::uint8_t exchange(std::uint8_t) noexcept override;
    void select(bool active) noexcept override;
    std::uint64_t milliseconds() noexcept override;
    void yield() noexcept override;
    void initializing(bool value) noexcept override { slow_ = value; }
  private:
    resources::BoardStorage configuration_{};
    bool slow_{true};
};

// Optional media never prevents internal storage startup. Mounts existing FAT
// SD/MMC volumes; only a completely erased dedicated NOR chip may be formatted.
class EspExternalStorage {
  public:
    explicit EspExternalStorage(resources::BoardStorage configuration = {}) noexcept
        : configuration_(configuration), software_card_(software_bus_) {}
    [[nodiscard]] bool mount() noexcept;
    [[nodiscard]] bool unmount() noexcept;
    [[nodiscard]] bool mounted() const noexcept { return mounted_; }
    [[nodiscard]] const char* state() const noexcept { return state_; }
    [[nodiscard]] const char* media() const noexcept;
    [[nodiscard]] SdSpiCard& software_card() noexcept { return software_card_; }
    [[nodiscard]] std::mutex& disk_mutex() noexcept { return disk_mutex_; }
  private:
    bool mount_software_sd() noexcept;
    bool mount_spi() noexcept;
    bool mount_mmc() noexcept;
    resources::BoardStorage configuration_{};
    GpioSdSpiBus software_bus_{};
    SdSpiCard software_card_;
    std::mutex disk_mutex_{};
    void* card_{};
    esp_flash_t* flash_{};
    const esp_partition_t* partition_{};
    void* fat_{};
    std::array<char, 4> drive_{};
    unsigned char drive_number_{0xff};
    int host_{-1};
    bool mounted_{};
    const char* state_{"not-probed"};
};
} // namespace blip::storage
