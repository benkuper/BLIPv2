#pragma once

#include "blip/core/component.hpp"
#include "blip/power/battery_model.hpp"

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_oneshot.h"

#include <cstdint>

namespace blip::power {

// HUZZAH32 ADC1/GPIO35 source. Other board sources need measured dividers.
class EspBatteryComponent final : public core::Component {
  public:
    [[nodiscard]] const core::ComponentDescriptor& descriptor() const noexcept override;
    [[nodiscard]] core::Status start(const core::StartContext&) noexcept override;
    [[nodiscard]] core::Status stop() noexcept override;
    [[nodiscard]] core::Status read_parameter(std::string_view id,
                                              core::ScalarValue& output) noexcept override;
    [[nodiscard]] core::Status write_parameter(std::string_view id,
                                               const core::ScalarValue& value) noexcept override;

  private:
    [[nodiscard]] bool sample() noexcept;
    static const core::ComponentDescriptor descriptor_;

    BatteryModel model_{};
    adc_oneshot_unit_handle_t unit_{};
    adc_cali_handle_t calibration_{};
    adc_channel_t channel_{};
    std::int64_t last_sample_us_{};
    std::uint32_t sample_errors_{};
    bool started_{};
};

} // namespace blip::power
