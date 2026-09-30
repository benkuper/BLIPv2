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

The HUZZAH32 test strip is wired to GPIO15. The strip owns this pin in the
manifest and the ESP32 build defaults to it. GPIO15 is also a boot strap: its
external LED data input must remain high impedance at reset. Pulling it low
silences the ESP32 ROM boot log.

The M5Dial build raises GPIO46 to hold battery power. GPIO38 supplies a single
status LED on its Stamp S3A controller module; the firmware drives its power
switch low while the RMT strip component is disabled. A visible light was
reported even while serial control reported `enabled=false`, so that light's
source still needs identification. The visible ring is the rotary control around
the circular LCD, not an addressable LED ring. The LCD and encoder still need
their V2 components.

`creators-ball-v2.json` records the separate 8 MiB ESP32-C6 board wiring from
the pinned BLIP V1 `creatorsballv2` configuration. Build it with
`-DBLIP_BOARD_CREATORS_BALL_V2=ON`. GPIO3 is HD108 data, GPIO2 is clock, GPIO14
is battery charge sense, GPIO21 enables the LEDs, and GPIO22 holds power. The
XIAO RF-switch GPIO initialization is disabled in this build. The firmware
raises GPIO22 at the start of `app_main`. Its fixed 36-pixel HD108 output now
uses SPI2 with DMA on GPIO3/GPIO2 and controls LED power with GPIO21. Protocol,
pin, and pixel count are fixed for this board. Hardware color and Gate C timing
still require qualification.

`creators-tab.json` records the separate ESP32 Creators Tab wiring from V1
`creatorstab`. Build it with `-DBLIP_BOARD_CREATORS_TAB=ON`. GPIO25 is the
WS2812B data pin, GPIO27 enables strip power, and GPIO12 holds board power.
The firmware raises GPIO12 at the start of `app_main` before service startup.
The V1 strip has 200 pixels with BGR ordering. The V2 output component now
controls GPIO27 strip power, but its current WS2812 encoder uses GRB order and
has not been qualified for that strip. The external strip was moved to the
HUZZAH32, so the Tab output currently has no visual LED test.

`m5stack-m5stickc.json` uses the M5Stack StickC pin map and the dormant V1
M5StickC profile. Build it with `-DBLIP_BOARD_M5STICKC=ON`. The current pixel
component defaults to the free external HAT GPIO26 instead of GPIO23, which is
the display DC line. The onboard red LED is a separate GPIO10 device; the
display, power manager, buttons, and IMU still need their V2 components.
