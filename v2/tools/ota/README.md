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

Prefer shared Wi-Fi for OTA HIL. This computer has one Wi-Fi interface, so a
device fallback AP removes its Internet route. If an AP test is necessary, run
the complete offline sequence in one local process with an independent timed
recovery process armed before changing networks. Reconnection in `finally` alone
cannot recover from a killed or hung test process.

`blip_ota_ap_hil.ps1` arms that recovery process, then restores and verifies
Internet access on the normal path. The independent process reconnects the
Internet profile if the test process dies or exceeds its 600-second default
deadline. Example:

```powershell
pwsh -File v2/tools/ota/blip_ota_ap_hil.ps1 -Operation status `
  -DeviceSsid BLIP-123456 -InternetProfile 'Archi-wifi guest'
```

`blip_ota_serial_capture.py` can run beside the AP operation to preserve boot
and rollback evidence. It leaves DTR/RTS inactive by default; use `--reset` when
a native USB console needs an explicit reset-and-release pulse after opening.
