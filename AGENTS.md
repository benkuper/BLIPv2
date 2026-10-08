# Repository instructions

## Testing a BLIP device access point

- A PC connected to a BLIP setup AP may lose Internet access. The user is not necessarily at the computer to recover it.
- Before changing the PC's Wi-Fi connection, record the current SSID and profile, then arm an independent, timed recovery process that will reconnect that profile even if the agent, command session, or Internet connection stops. Do not rely only on a `finally` block or on a later agent action for recovery.
- Keep the recovery process armed until the original Wi-Fi profile is reconnected and Internet access has been verified. Reconnect the original profile promptly after testing, then remove any temporary BLIP profile and cancel the recovery process.
- If an independent automatic recovery process cannot be armed and verified, do not switch the PC to a BLIP AP. Use serial, a separate network adapter, or another test method.
- The AP HIL wrappers in `v2/tools/control/` and `v2/tools/ota/` arm `v2/tools/wifi/blip_wifi_recovery.ps1` before any disconnect. Keep this safeguard in place when changing those scripts.
- For shared-network tests, provision boards over serial from `v2/firmware/local-wifi.txt` using the repository's Wi-Fi provisioning helper. This local credentials file is ignored by Git; never copy its password into tracked documentation or logs.

## Connected test boards

- The user authorizes flashing the connected boards and does not require backups or restoration of their previous firmware. Leave validated test firmware installed unless a particular test or a later user instruction requires another image.
- The M5Dial's native USB can remain in download mode after a flash/reset. Use esptool's default stub-assisted `run` and verify application readiness over serial. `--no-stub run` did not reliably start this test board. Production S3 builds use `sdkconfig.esp32s3.defaults` to select the native USB console; flash the matching bootloader when changing console configuration.
