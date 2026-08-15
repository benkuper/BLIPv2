# BLIP V2 browser installer

The builder creates one merged 4 MiB factory image for ESP32, ESP32-S3, and
ESP32-C6, an ESP Web Tools manifest, an ESP Launchpad TOML configuration, SHA-256
provenance, and the HTTPS-hosted installer page. It includes the initialized
LittleFS image at `0x340000`; OTA application uploads intentionally do not touch
that partition.

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

The current firmware provisions through its device-hosted schema UI after the
browser flash. It does not claim Improv Serial support, so the manifest declares
`improv: false` and disables the post-install Improv wait.
