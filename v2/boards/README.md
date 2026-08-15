# Board manifests

Schema-version-1 manifests bind verified board pin inventories to the reference
targets. Each pin has a stable resource ID, board label, GPIO, capability set,
electrical note, and any system reservation/bus role. The firmware-compiled
representation is `blip/resources/board_manifest.hpp`; the JSON files are the
human-readable Kitchen inputs and are validated in CI.

Reference mappings are Adafruit HUZZAH32 (`esp32`), M5Stack M5Dial (`esp32s3`),
and Seeed XIAO ESP32-C6 (`esp32c6`). The C6 reference profile is explicitly the
onboard/chip-antenna variant: GPIO3 and GPIO14 remain visible but reserved for
the RF switch, and the Wi-Fi antenna parameter remains configurable.
