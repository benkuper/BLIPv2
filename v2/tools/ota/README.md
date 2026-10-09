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

## Native release recovery on the existing LAN

`blip_release_recovery_hil.py` runs a temporary public local catalog/artifact
server and injects catalog, identity, hash, truncation, web CRC, cancellation and
reset faults. Both candidate artifacts must advance the installed codes. Supply
an archive of the exact installed initial app, the newer build, a matching web
bundle, the verified MAC/flash log, serial port and the PC's existing LAN IP:

```powershell
python v2/tools/ota/blip_release_recovery_hil.py --port COM_PORT --mac DEVICE_MAC `
  --listen PC_LAN_IP --server-port 8091 --build build/CANDIDATE `
  --initial-build build/INSTALLED --flash-log build/FLASH_LOG `
  --bundle build/NEW_WEB.bundle --report build/recovery.json
```

Optional `--playwright PATH/index.mjs --browser PATH/browser.exe` opens a real
headless browser on the device. `--hold-http-sessions 3` fills the other HTTP
session slots. `--skip-faults` isolates valid web installation/browser recovery;
in this mode firmware need not advance. Every web candidate must still be newer.
These are actual writes and stub-assisted resets; leave matching validated
firmware installed. The tool restores update policy and records original policy,
source/image/tool hashes, per-fault checks and browser/serial diagnostics.
It never changes PC Wi-Fi or provisions credentials.

The standalone public release handler and local simulator use independent HTTP
workers so a device writing flash does not block another device's catalog check.
`blip_release_firmware_hil.py` qualifies a valid native firmware upgrade, journals
the original policy and reopens transient native-USB serial handles without an
extra reset. It retains failed reports rather than treating an interrupted
checker as successful installation evidence.
