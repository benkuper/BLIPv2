# BLIP V2 browser installer

The builder creates merged factory images, an ESP Web Tools manifest, an ESP
Launchpad TOML configuration, SHA-256 provenance, and the HTTPS-hosted installer
page. The generic three-target package uses the 4 MiB layout with initialized
LittleFS at `0x340000`; OTA application uploads do not touch that partition.

Run after all three ESP-IDF builds complete:

```powershell
python v2/installer/build_browser_installer.py `
  --esp32-build build/esp32 --esp32s3-build build/esp32s3 `
  --esp32c6-build build/esp32c6 --output build/browser-installer `
  --base-url https://downloads.example/blip-v2/0.1.0
```

Host the output with HTTPS and permissive CORS headers. `manifest.json` is
consumed by ESP Web Tools (esptool-js); `launchpad.toml` can publish the same
single-bin artifacts in ESP Launchpad. Factory installation erases the device
by default and is distinct from the on-device A/B application updater.

The opt-in C6 BLE builds use board-specific layouts. Create a separate package
for each board, then publish each package at a clearly named URL:

```powershell
python v2/installer/build_browser_installer.py `
  --board-id creators-ball-v2 --board-build build/m5-ble-ball-8mb `
  --output build/ball-ble-installer --base-url https://downloads.example/blip-v2/ball-ble
python v2/installer/build_browser_installer.py `
  --board-id seeed-xiao-esp32c6-chip-antenna --board-build build/m5-ble-xiao-c6-ota `
  --output build/xiao-ble-installer --base-url https://downloads.example/blip-v2/xiao-ble
```

The Ball package uses 8 MiB flash and LittleFS at `0x620000`; the XIAO package
uses 4 MiB and LittleFS at `0x360000`. The builder checks the build target,
BLE profile, board selection, partition filename, flash size, and storage offset.
Both packages identify as ESP32-C6 to Web Serial, which cannot distinguish the
boards by chip family. Select the package for the physical board before flashing.

The current firmware provisions through its device-hosted schema UI after the
browser flash. It does not claim Improv Serial support, so the manifest declares
`improv: false` and disables the post-install Improv wait.
