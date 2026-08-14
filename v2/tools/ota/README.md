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
