# Firmware application

This is the ESP-IDF 6.0.2 application composition root. Work package 1.1 is a
minimal C++20 bootstrap only; it contains no production subsystem or hardware
configuration.

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

The committed custom partition table is sized for a 2 MiB device and reserves
an 896 KiB `storage` LittleFS partition. The internal storage component formats
that partition only when it is completely erased; unrecognized nonblank media
fails closed so legacy data is not silently destroyed.
