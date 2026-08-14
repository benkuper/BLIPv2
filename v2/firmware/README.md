# Firmware application

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

The startup task has an 8 KiB bounded stack so the depth-limited V1 settings
parser can run before registry confirmation. ESP-IDF releases this transient
stack after `app_main` returns; runtime components do not retain it.

The composition root will remain thin: reusable production behavior belongs in
the component directories under [`../components/`](../components/).

After boot confirmation, parameters and actions are available through the
COBS-framed BLIP envelope on UART0 (ESP32) or native USB Serial/JTAG (S3/C6).
The host client and examples are in
[`../tools/control/`](../tools/control/README.md).

The committed custom partition table is sized for a 2 MiB device and reserves a
1.5 MiB factory application, a 384 KiB `storage` LittleFS partition, and a 64 KiB
`coredump` partition. The internal storage component formats its partition only when it is
completely erased; unrecognized nonblank media fails closed so legacy data is
not silently destroyed. Coredumps are validated at boot, retained without
overwrite, and erased only by an explicit diagnostics action.
