# ADR-0007: Milestone 3 LED transport baseline

- Status: Accepted for the Milestone 3 vertical slice
- Date: 2026-08-15
- Owners: BLIP V2 LED/output

## Context

Milestone 3 needs one native WS2812/SK6812 strip on ESP32, ESP32-S3, and
ESP32-C6. That functional slice must not silently decide the production
transport architecture. Candidate mechanics include RMT, RMT with DMA, SPI
waveform expansion with and without DMA, native clocked SPI, multiline SPI,
and each target's parallel engine.

The disposable harness at
[`v2/qualification/led_transport`](../../../v2/qualification/led_transport/)
was built with ESP-IDF 6.0.2 and run on one physical device per target. Each
run requested 1, 2, 4, 8, and 16 lanes and 1, 32, 256, 512, and 1,024 pixels,
using eight frames per completed row. The original two-megabyte overwrite
range was restored and verified after every run. Each target emitted all 115
expected records.

## Measurements

The following values are wall time for eight 1,024-pixel, one-lane frames.
They include transport completion but exclude application scheduling and, for
SPI, the separately measured waveform encoder time.

| Target | Native RMT | RMT DMA | SPI waveform DMA | SPI encoder | Clocked SPI |
| --- | ---: | ---: | ---: | ---: | ---: |
| ESP32 | 255,991 us | unavailable | 243,506 us | 40,585 us | 26,833 us |
| ESP32-S3 | 255,926 us | 256,537 us | 243,524 us | 33,064 us | 26,864 us |
| ESP32-C6 | 255,841 us | unavailable | 243,478 us | 24,293 us | 26,815 us |

Native RMT completed the 1,024-pixel sweep with up to eight allocated lanes on
ESP32, four on S3, and two on C6. This proves allocation and completion, not
simultaneous start or skew. With the harness's conservative 512-symbol DMA
buffer, S3 RMT DMA completed one lane but a second allocation failed. It gave
no single-lane wall-time improvement over ordinary RMT.

SPI waveform transmission without DMA completed the 9-byte one-pixel row, then
failed once the polling transaction exceeded the driver's non-DMA transaction
limit. The DMA path completed all sizes but required nine staging bytes per RGB
pixel and an explicit encoding pass. Native clocked SPI required 4,164 bytes
for the 1,024-pixel APA102-family frame and completed far below its physical
clock budget.

The complete JSON rows and filtered serial records are in
[`docs/v2/evidence/led-transport`](../evidence/led-transport/2026-08-15-transport-qualification.json).

## Decision

Use ordinary native RMT, without DMA, for the Milestone 3 single-strip
WS2812/SK6812 vertical slice on all three targets. It has no application-side
waveform expansion buffer, uses the same public ESP-IDF driver contract on all
targets, and its measured single-lane completion follows the one-wire physical
frame period. This selection is deliberately limited to one short functional
strip and is not the production `auto` backend policy.

Retain SPI DMA as the leading one-wire candidate for long strips when staging
memory and encoder cost are acceptable. Use SPI DMA for native clocked strips.
Do not select S3 RMT DMA merely because it exists: buffer size, channel economy,
CPU/ISR behavior, and capture integrity must be retested together. Do not admit
multiline SPI, ESP32 I2S-LCD, S3 LCD_CAM, or C6 PARLIO into production until the
external capture fixture qualifies every claimed lane.

## Open qualification boundary

No logic analyzer or common external strip fixture was available. Therefore
the current run does not claim byte decoding, pulse tolerances, corruption
rate, lane skew, or behavior under saturated Wi-Fi, settings/metrics traffic,
flash/cache-disable windows, OTA, PSRAM, or full-profile resource pressure.
Those rows remain mandatory before Gate C production selection. Work package
3.4 may proceed only with the single-lane RMT boundary above; its own host
goldens and physical-strip capture remain acceptance requirements.

## Required short backend check

Before a backend is proposed for production selection it must, at minimum:

1. complete the 1/32/256/512/1,024-pixel sweep at every claimed lane count;
2. decode a counter plus walking-bit payload with zero byte errors;
3. keep every high/low pulse inside the protocol tolerance and report worst
   observed deviation and lane-start skew;
4. repeat while Wi-Fi is saturated and while bounded settings/flash work runs;
5. report staging/DMA/internal/PSRAM bytes, encoder time, ISR/refill metrics,
   missed deadlines, queue behavior, and every brokered resource; and
6. restore and verify the original firmware/storage ranges after disposable HIL.

## Consequences

Milestone 3 gains a portable, low-staging native vertical slice without
prejudging Milestone 4. The evidence explicitly distinguishes API completion
from waveform qualification, so an unavailable fixture cannot become an
implicit pass.
