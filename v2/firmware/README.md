# Firmware application

Each board publishes its human-readable type and a saved name defaulting to that
type. Both web modes show the identity and provide a name editor. mDNS derives its
hostname from the name plus a stable MAC suffix and follows saved changes live.
Original-ESP32 WASM profiles keep ordinary SPI completion interrupts in flash to
preserve a contiguous 64 KiB guest arena. Optional MMC/NOR storage providers are
included only when selected; current external-media boards use SD SPI.

This is the ESP-IDF 6.0.2 application composition root. It remains intentionally
thin while composing the registry, diagnostics, storage, serial control, and Wi-Fi
services implemented by the component libraries.

After activating an ESP-IDF 6.0.2 environment, build one initial target from
the repository root with:

```powershell
idf.py -C v2/firmware -B build/esp32 -D IDF_TARGET=esp32 build
python v2/tests/host/validate_firmware_build.py --build-dir build/esp32 --target esp32
```

Replace `esp32` with `esp32s3` or `esp32c6` for the other profiles. Generated
configuration stays in the selected build directory. The dependency resolver
uses a separate committed lock for each target.

The full web interface is packaged directly into `storage.bin` during the
build. A normal `idf.py ... -p PORT flash` installs it with the firmware; no
manual file upload or separate web build is needed. The application binary
contains no copy of the full interface, so it does not occupy either OTA slot.
Application-only OTA preserves the independently versioned filesystem bundle.

Use `-DBLIP_FLASH_WEB_UI=OFF` to create an empty factory filesystem instead.
The device then serves its small gzip-compressed first-run page, downloads the
interface from the configured public release endpoint, installs it atomically
and refreshes into the full app. `/firstrun` and `/setup` remain available for
retry and network setup. Both factory modes use the same application binary.

Plain HTTP and no additional application authentication are the standard
private show-network defaults. Optional `-DBLIP_ENABLE_RELEASE_TLS=ON` builds
add HTTPS, its CA bundle and clock synchronization. See
[ADR-0020](../../docs/v2/adr/0020-private-show-network-defaults.md) and the
[local release simulator](../tools/ota/RELEASE_SERVER.md).

Art-Net discovery/DMX and DDP pixel input are enabled by default. Pass
`-DBLIP_ENABLE_ARTNET=OFF` or `-DBLIP_ENABLE_DDP=OFF` to `idf.py` to exclude
either protocol component and its registry entry from a smaller build. Use a
separate build directory for each option combination.
The E1.31 receiver is optional and disabled by default; pass
`-DBLIP_ENABLE_E131=ON` to include it.

OSC and OSCQuery advertise through mDNS/Zeroconf as `_osc._udp` (9000) and
`_oscjson._tcp` (80). Their shared hostname is `blip-<STA MAC without colons>.local`;
service instances include the MAC so several BLIPs can be discovered together.
Records are withdrawn when OSC or the HTTP server stops and recreated when the
network server returns, including after a Wi-Fi restart.

Art-Net uses native ArtPoll/ArtPollReply discovery. Replies describe one output
on Port-Address 0, with a unique port name, current IP/MAC, DHCP status and web
configuration capability. Targeted polls are filtered; up to four controllers
can request change notifications, with a 10-second subscription expiry and
randomized replies within approximately one second. Replies are unicast under
the current [Art-Net specification](https://art-net.org.uk/downloads/art-net.pdf).
GoodOutput indicates accepted ArtDmx traffic within the last 2.5 seconds.

DDP supports native STATUS (251) discovery queries and read-only CONFIG (250)
queries on UDP 4048, including broadcast requests. CONFIG reports the current
RGB8 pixel/channel count and destination 1. Queries do not alter pixel data or
sequence tracking; configuration writes remain unsupported. sACN/E1.31 is a
receiver here: its [Universe Discovery](https://tsp.esta.org/tsp/documents/docs/E1-31-2016.pdf)
advertises transmitting sources, so this receiver does not emit those packets.

Discovery validation is recorded in the
[2026-10-08 evidence report](../../docs/v2/evidence/discovery/2026-10-08-discovery.json):
host tests, target builds, shared-LAN discovery/reconnect checks on C6/ESP32/S3,
and simultaneous lighting traffic with full OSCQuery reads on the BLE/WASM Ball.

For the Adafruit HUZZAH32 battery ADC on GPIO35, pass
`-DBLIP_BOARD_ADAFRUIT_HUZZAH32=ON` in a dedicated ESP32 build directory.
The battery registry entry is included only in that board build.
For the Creators Club, pass `-DBLIP_BOARD_CREATORS_CLUB=ON` in a dedicated
ESP32 build directory. Its fixed SK9822 output uses SPI2 on GPIO25/GPIO26.

The startup task has an 8 KiB bounded stack. ESP-IDF releases this transient
stack after `app_main` returns; runtime components do not retain it.

Bulk storage automatically prefers mounted media declared by the board manifest.
The Ball's SD-protocol onboard storage uses software SPI so its HD108 DMA host
stays available; Club/Tab SD use SPI3. Other boards retain internal LittleFS.
Web assets, scripts, playback and sequences share the same bounded file service;
see the [file API and hardware checks](../tools/storage/README.md). The factory
UI is copied to external storage through this service when available.

The composition root will remain thin: reusable production behavior belongs in
the component directories under [`../components/`](../components/).

After boot confirmation, parameters and actions are available through the
COBS-framed BLIP envelope on UART0 (ESP32) or native USB Serial/JTAG (S3/C6).
The host client and examples are in
[`../tools/control/`](../tools/control/README.md).

The committed custom partition table targets 4 MiB devices and reserves two
1.5625 MiB OTA application slots, 640 KiB `storage` LittleFS, a redundant 8 KiB OTA
selection ledger, and 128 KiB `coredump`. The internal storage component formats
its partition only when it is completely erased; unrecognized nonblank media
fails closed so unknown data is not silently destroyed. Coredumps are validated
at boot, retained without overwrite, and erased only by an explicit diagnostics
action.

This is a partition migration from work package 3.2 and requires a full-flash
backup plus a full bootloader/partition/application install. Do not flash only
the 3.3 application into the former factory slot. OTA usage and rollback details
are recorded in [work package 3.3](../../docs/v2/work-packages/3.3-ab-ota-rollback.md).
