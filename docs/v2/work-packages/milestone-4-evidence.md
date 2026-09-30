# Milestone 4 evidence

## Automated evidence

The `blip_led_m4_tests` executable covers the output ABI, physical-period and
memory calculations, compositor priority and blend vectors, color correction,
APA102/SK9822/HD108 and one-wire/parallel encoding, fixed frame pools, overload,
deadlines, deterministic selection, sequence-aware streaming, V1 playback
import, corrupt playback rejection, Art-Net discovery/DMX, E1.31, and DDP.

Its sustained case interleaves 50,000 Art-Net updates and frame submissions and
requires zero queue rejections, zero drops, a queue high-water of one, and exact
completion counts. The complete 16-test host CTest suite passes with warnings as
errors. Minimal firmware builds pass for ESP32, ESP32-S3, and ESP32-C6. For
Creators Ball V2 (ESP32-C6), all four combinations of the Art-Net and DDP
build switches pass, along with E1.31-only and all-three configurations.
ESP-IDF's component list contains exactly the enabled network lighting
components. E1.31 has a registered optional UDP runtime, and
the three-protocol image builds for all six boards. The E1.31-only C6 image
contains just `blip_e131`; the all-off image contains no network lighting
component. Ball E1.31 unicast and multicast results are in
`docs/v2/evidence/board-bringup/2026-10-01-creators-ball-e131.json`, and the
six-board build hashes are in
`docs/v2/evidence/board-bringup/2026-10-01-e131-six-board-builds.json`.
Short Ball Art-Net discovery/DMX hardware results, including the user's
visual observation, are recorded in
`docs/v2/evidence/board-bringup/2026-10-01-creators-ball-artnet.json`.
The concurrent shared-Wi-Fi Ball run is recorded in
`docs/v2/evidence/board-bringup/2026-10-01-ball-network-soak.json`. Over a
requested 180 seconds, the sender transmitted 3,687 packets each of Art-Net,
DDP, and E1.31 at 20 frames/s per protocol to 36 HD108 pixels. Device counters
rose by 3,674, 3,673, and 3,671 accepted packets respectively; all three
rejected counters and the LED output failure counter stayed at zero. Applied
frames rose by 3,216. Internal free heap was 224,424 bytes at both ends; its
minimum fell to 172,612 bytes early in the run and stayed there. This short
sample does not establish long-term heap stability. The PC remained on
`Archi-wifi guest` and its Internet connection was verified afterward.
The default Art-Net/DDP image also builds on all six board definitions, as
recorded in `docs/v2/evidence/board-bringup/2026-10-01-artnet-ddp-six-board-builds.json`.

## Gate C boundary

Gate C is intentionally not marked complete by software builds or host soak.
ADR-0007 still requires external waveform capture and sustained on-device Wi-Fi,
flash/settings, and metrics load on every claimed backend/target range. Until
that evidence exists, the ESP RMT/RMT-DMA and SPI-DMA capability records expose
`qualified=false`, and both automatic and forced selection reject them. This is
a fail-closed production boundary, not an implicit qualification waiver.

Run the host evidence with:

```powershell
cmake -S v2/tests/host -B build/host-m4
cmake --build build/host-m4 --config Debug --parallel
ctest --test-dir build/host-m4 -C Debug --output-on-failure
```
