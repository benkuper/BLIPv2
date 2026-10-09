# Repository instructions

## Testing a BLIP device access point

- A PC connected to a BLIP setup AP may lose Internet access. The user is not necessarily at the computer to recover it.
- Before changing the PC's Wi-Fi connection, record the current SSID and profile, then arm an independent, timed recovery process that will reconnect that profile even if the agent, command session, or Internet connection stops. Do not rely only on a `finally` block or on a later agent action for recovery.
- Keep the recovery process armed until the original Wi-Fi profile is reconnected and Internet access has been verified. Reconnect the original profile promptly after testing, then remove any temporary BLIP profile and cancel the recovery process.
- If an independent automatic recovery process cannot be armed and verified, do not switch the PC to a BLIP AP. Use serial, a separate network adapter, or another test method.
- The AP HIL wrappers in `v2/tools/control/` and `v2/tools/ota/` arm `v2/tools/wifi/blip_wifi_recovery.ps1` before any disconnect. Keep this safeguard in place when changing those scripts.
- For shared-network tests, provision boards over serial from `v2/firmware/local-wifi.txt` using the repository's Wi-Fi provisioning helper. This local credentials file is ignored by Git; never copy its password into tracked documentation or logs.

## Connected test boards

- Milestone completion must be validated on all six required device types: Creators Club, Creators Ball V2, HUZZAH32, XIAO C6, M5StickC, and M5Dial. Routine development checks may target affected boards; builds alone do not count as hardware validation. Record per-board results and outstanding failures before claiming a milestone complete. Reverify serial ports and device identities rather than assuming port assignments remain stable.
- As of 2026-10-09 the user has no working Creators Tab and excludes it from all checks. Do not include it in build/test/qualification matrices, probe or flash it, or treat its absence/failure as a milestone blocker. Historical evidence and its board implementation remain available.
- Device identity must expose the human-readable board type and a persisted, editable name defaulting to that type. Display both in the web interface. mDNS hostnames derive from the name with a stable uniqueness suffix, and discovery must follow saved name changes.
- The user authorizes flashing the connected boards and does not require backups or restoration of their previous firmware. Leave validated test firmware installed unless a particular test or a later user instruction requires another image.
- Creators Ball V2 has 8 MiB flash (esptool `8MB`), recorded as `flash_bytes: 8388608` in its board manifest. Use its `sdkconfig.ball-8mb.defaults` and `partitions-8mb.csv` for factory/full flashes, and honor the generated flash arguments rather than assuming the XIAO C6's 4 MB layout.
- The M5Dial's native USB can remain in download mode after a flash/reset. Use esptool's default stub-assisted `run` and verify application readiness over serial. `--no-stub run` did not reliably start this test board. Production S3 builds use `sdkconfig.esp32s3.defaults` to select the native USB console; flash the matching bootloader when changing console configuration.
