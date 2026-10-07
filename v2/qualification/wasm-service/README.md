# WASM service qualification

This standalone project links the actual `blip_wasm` service and WAMR adapter.
It is separate from the immutable 6.1 engine benchmark. It has no LED, radio,
settings, storage or production control tasks and cannot qualify Gate D.

## Build

Activate ESP-IDF 6.0.2, prepare the pinned runtime checkout with
`python v2/qualification/wasm/prepare_dependencies.py`, then:

```powershell
idf.py -C v2/qualification/wasm-service -B build/m6-service-xiao -D IDF_TARGET=esp32c6 build
idf.py -C v2/qualification/wasm-service -B build/m6-service-m5dial -D IDF_TARGET=esp32s3 build
idf.py -C v2/qualification/wasm-service -B build/m6-service-huzzah32 -D IDF_TARGET=esp32 build
```

The generated `main/fixtures.hpp` is checked in. Regeneration requires host-only
`wasmtime==36.0.0` and `generate_fixtures.py`; the target does not use Wasmtime.
The runtime checkout must match the accepted commit and be unmodified. The
recipe uses instruction metering, software bounds checks and the internal
thread manager for synchronized cancellation; guest threading stays disabled.

## Run

```powershell
python v2/qualification/wasm-service/run_service.py --port COM13 --chip esp32c6 --board seeed-xiao-esp32c6 --build build/m6-service-xiao --report build/m6-service-xiao.json
```

Only the app at `0x20000` and OTA selection at `0x18000` are written. PC Wi-Fi,
bootloader, partitions, NVS and storage are untouched. The user designates these
as testing boards, so the default leaves the test image installed. A supplied
`--restore build/production-image` enables restoration and serial verification
in `finally`. Forced process termination cannot execute that optional cleanup.

The worker uses a 96 KiB fixed engine pool, 16 KiB module scratch, 4 KiB Wasm
stack and 16 KiB native stack. A higher-priority supervisor cancels a tight loop
after 5 ms and resets on a 100 ms unresponsive call. Such a reset fails the run.
Loading rejects start sections and WAMR automatic constructors before guest
execution. Tests exercise mixed numeric/multiple results, memory initialization
and growth, invalid requests, traps, cancellation, recovery by reload,
100 load/unload cycles and 20 full start/stop cycles. SDK heap after warmup and
unloaded pool use must remain exactly constant across the repeated cycles.

Reports bind results to source, fixture, image, ELF/map/config and dependency
hashes. They retain failures and console output even if the trial fails. A
passing trial is a service/adapter qualification, not seven-board production
compatibility or proof of wall-clock isolation alongside real firmware tasks.
