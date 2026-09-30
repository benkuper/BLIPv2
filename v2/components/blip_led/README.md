# blip_led

The Milestone 3 vertical slice provides one persisted WS2812/SK6812 strip as
`blip.output.strip0`. The portable model defines GRB/GRBW order, brightness,
bounded settings, and 10 MHz RMT timings. The ESP-IDF adapter owns one native
RMT TX channel, a fixed 4 KiB pixel buffer, and a static output task.

Color/settings writes schedule the latest complete frame; the control callback
does not wait for the one-wire transfer. Pin and protocol changes require the
strip to be disabled, and enable/reconfiguration rolls platform state back if
persistence or RMT setup fails. The component uses ordinary RMT without DMA as
limited by [ADR-0007](../../../docs/v2/adr/0007-led-transport-qualification.md).

`BLIP_LED_DEFAULT_GPIO` supplies a board build's initial pin. It does not
override persisted settings.

## Milestone 4 engine

The production engine is split into portable contracts and ESP-IDF ports:

- `engine.hpp` defines linear surfaces and the output-driver capability ABI,
  including formats, lanes, clock/timing limits, physical period, buffering,
  DMA, synchronization, staging memory, qualification, and resource claims.
- `compositor.hpp` fixes priority as stream, playback, script, then system.
- `color.hpp` applies a signed Q16.16 matrix and calibration in linear light
  before channel ordering and linear/sRGB/gamma-2.2 quantization.
- `frame_pipeline.hpp` owns aligned, fixed-size staging slots and records queue,
  failure, completion, deadline, drop, and rejection metrics.
- `protocol_encoder.hpp` encodes WS2812/SK6812 payloads, three-bit SPI one-wire
  waveforms, APA102/SK9822 frames, and 64-bit HD108 pixel frames.
- `parallel_encoder.hpp` transposes up to sixteen lane payloads into deterministic
  three-phase bit-plane samples for target parallel peripherals.
- `esp_rmt_output_driver.hpp` and `esp_spi_dma_output_driver.hpp` implement the
  common asynchronous ABI. `port_adapters.hpp` is the target-neutral seam for
  I2S/LCD, LCD_CAM, PARLIO, and optional FastLED ports.
- The Creators Ball V2 composition drives its fixed 36-pixel HD108 chain through
  SPI2 DMA and GPIO21 LED power. Its data, clock, and count cannot be changed.
- The native RMT component uses the same stream layer and compositor priority
  for its configured WS2812/SK6812 pixels. It encodes GRB or GRBW under a
  stream mutex, then submits the bounded frame without holding that mutex.
  A stream expires after one second without packets and clears the output.
- `stream.hpp` and `playback.hpp` provide bounded sequence-aware streaming,
  strict versioned playback and deterministic playback time. The V1 ARGB
  conversion helper remains in the source tree as historical tooling and is
  outside the V2 release scope.

The selector will not use a backend whose capability record is unqualified,
even when forced. ESP transport records therefore remain fail-closed until the
required Gate C capture and sustained-load evidence is attached; compilation
or host protocol goldens do not silently count as physical qualification.
