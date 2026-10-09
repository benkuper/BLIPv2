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

`creators-ball-v2.json` records `flash_bytes: 8388608` (8 MiB, esptool `8MB`) and the ESP32-C6 board wiring from
the pinned BLIP V1 `creatorsballv2` configuration. Build it with
`-DBLIP_BOARD_CREATORS_BALL_V2=ON`. The build selects `sdkconfig.ball-8mb.defaults`
and `partitions-8mb.csv`; an existing configuration with a different flash size
or partition table is refused. The browser installer also verifies the 8 MB
layout. GPIO3 is HD108 data, GPIO2 is clock, GPIO14
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

`creators-club.json` records the ESP32 Creators Club wiring from V1
`creatorsclub`. Build it with `-DBLIP_BOARD_CREATORS_CLUB=ON`. GPIO12 holds
power, GPIO27 enables the strip, and SPI2 drives SK9822 data/clock on
GPIO25/GPIO26. The fixed 32 logical pixels each expand to three physical LEDs
in reverse logical order, matching V1's `LED_LEDS_PER_PIXEL=3` and
`LED_DEFAULT_INVERT_DIRECTION=true`. The V1 default power budget is 1200 mA
and its 0.4 brightness factor maps to V2 brightness 102/255. The Club's SD
uses the automatic file service on its reserved SPI3 pins and active-low power
enable. IMU, button, battery, and IR drivers remain pending. V1 lists GPIO3 as charge sense, overlapping
the ESP32 UART0 RX used by the USB serial bridge, so no charge sense driver
claims that pin.
The V2 full-strip red, green, and blue check was visually confirmed on
2026-10-01; the SPI driver reported zero failed frames. Timing captures and
physical current calibration remain separate qualification work.

`m5stack-m5stickc.json` uses the M5Stack StickC pin map and the dormant V1
M5StickC profile. Build it with `-DBLIP_BOARD_M5STICKC=ON`. The current pixel
component defaults to the free external HAT GPIO26 instead of GPIO23, which is
the display DC line. The onboard red LED is a separate GPIO10 device; the
display, power manager, buttons, and IMU still need their V2 components.
