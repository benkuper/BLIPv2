# V1 source baseline and evidence policy

## Immutable baseline

Milestone 0 is derived from the following source identity:

| Field | Value |
| --- | --- |
| Repository | `https://github.com/Golden-Geek/BLIP.git` |
| Commit | `e567eeb5f20ba022595fd5b89a77fb17ba59ca94` |
| Commit subject | `bit faster upload` |
| Firmware version macro | `1.2.0` |
| Build system | PlatformIO, Arduino framework, C++17 |
| Default environment | `creatorsballv2` |

The full 40-character commit is the source-of-truth identifier. Branch names,
tags, and the seven-character abbreviation are informational only.

## Evidence classes

Compatibility claims use one of these classes:

| Class | Meaning |
| --- | --- |
| `source-derived` | Reconstructed directly from the pinned code/configuration and independently parseable on a host. |
| `device-captured` | Bytes or output captured from a device running the pinned firmware, with board and build environment recorded. |
| `measured` | Timing, memory, electrical, or stability evidence collected with the documented test method. |
| `planned` | A contract or target without implementation evidence yet. |

The initial fixtures are `source-derived`, because no physical V1 device or
private device settings were provided. They are deliberately synthetic and
contain no real credentials, MAC addresses, or user content. Device captures
may be added later as separate fixtures; they must not replace these deterministic
source fixtures.

## Traceability

The main source locations used by this milestone are:

| Surface | Pinned source paths |
| --- | --- |
| Component lifecycle, parameters, settings tree, OSCQuery generation | `src/Component/Component.{h,cpp}`, `src/Common/{Helpers,Parameter,Settings}.{h,cpp}` |
| Root composition and routing | `src/RootComponent.{h,cpp}` |
| Serial grammar | `src/Component/components/communication/serial/SerialComponent.cpp`, `src/Common/StringHelpers.cpp` |
| OSC and mDNS | `src/Component/components/communication/osc/OSCComponent.{h,cpp}` |
| OSCQuery HTTP and WebSocket | `src/Component/components/communication/server/WebServerComponent.{h,cpp}` |
| ESP-NOW frames | `src/Component/components/communication/espnow/ESPNowComponent.{h,cpp}`, `src/Common/var.h` |
| Playback files | `src/Component/components/ledstrip/Layer/layers/playback/LedStripPlaybackLayer.{h,cpp}`, `src/Common/color.h` |
| Build features and boards | `platformio.ini`, `configs/*.ini` |

## Reproduction

Inspect the baseline without checking it out over this repository:

```powershell
git fetch https://github.com/Golden-Geek/BLIP.git e567eeb5f20ba022595fd5b89a77fb17ba59ca94
git show e567eeb5f20ba022595fd5b89a77fb17ba59ca94:src/RootComponent.cpp
```

Regenerate and validate the deterministic fixtures:

```powershell
python v2/tests/fixtures/v1/generate.py --check
python v2/tests/host/validate_foundation.py
```

Generation is allowed to overwrite only paths enumerated by the V1 fixture
manifest. The validator is offline and uses only the Python standard library.

## Gaps that require hardware

The following cannot honestly be claimed from static source inspection:

- exact OSCQuery output for every historical build-flag combination;
- NVS blobs written by each ArduinoJson/Arduino core version in the field;
- compiler-dependent struct padding on non-ESP32 V1 targets;
- RF timing, LED timing, boot duration, current draw, and heap behavior;
- filesystem state after real power interruption;
- unpublished playback, dashboard, or fleet packets.

These are tracked as evidence requirements in the quality gates and relevant
work packages. Unknown behavior is not treated as permission to break it.
