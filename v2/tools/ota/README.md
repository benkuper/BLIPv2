# Firmware OTA utility

Inspect an ESP-IDF application artifact and upload it to a device on a trusted
local network:

```powershell
python v2/tools/ota/blip_ota.py build/esp32/blip-v2.bin `
  --device http://192.0.2.8 --target esp32 --profile minimal
```

The utility derives the project and version from the image descriptor, computes
SHA-256 without loading the whole image, streams it to `PUT /api/firmware`, and
expects the device to restart. Use `--inspect` to perform only the local checks.
Use `--interrupt-after BYTES` for HIL testing of a deliberately truncated
transfer; the value must be between zero and the application image size.
The response timeout defaults to 300 seconds because application validation and
flash writes can exceed 30 seconds on a device AP; override it with `--timeout`.

## Device access-point HIL rule

This development computer has one Wi-Fi interface. Connecting it to a device
fallback AP removes its Internet route, so Codex cannot make another model/tool
round trip until normal Wi-Fi is restored. Never split an AP qualification
across interactive tool calls or depend on Codex to decide the next step while
the machine is connected to the device. Prewrite the complete offline sequence,
run it as one local process, and put restoration of the original Wi-Fi profile
in an unconditional cleanup (`finally`) path. The process must return only after
the development machine has reconnected to its original network.

`blip_ota_ap_hil.ps1` enforces that rule for the fallback AP. It freezes Windows
network use into one prewritten operation and restores the named Internet
profile before returning, including on failure. Example:

```powershell
pwsh -File v2/tools/ota/blip_ota_ap_hil.ps1 -Operation status `
  -DeviceSsid BLIP-123456 -InternetProfile MyWifi
```

`blip_ota_serial_capture.py` can run beside the AP operation to preserve boot
and rollback evidence. It leaves DTR/RTS inactive by default; use `--reset` when
a native USB console needs an explicit reset-and-release pulse after opening.
