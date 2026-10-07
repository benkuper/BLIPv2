# Production worker qualification

The native ESP-IDF application links the actual `EspWasmComponent` and pinned
WAMR adapter. It qualifies cold/idle/loaded/partial-upload start and stop,
active cancellation with eight requests queued, actual supervisor deadlines,
monotonic request IDs and twenty native task restarts. After idle task cleanup,
DRAM, IRAM and task counts must return exactly to their warmed baseline.

Linker wrappers exist only in this application. One-shot failures cover engine
and module allocation, the ESP32 linear arena, supervisor creation, both real
SDK pthread bookkeeping allocations, and the worker's FreeRTOS task creation.
Every fault must be consumed once, release resources and permit a fresh
upload/call/restart. An explicit backend initialization rejection also checks
the created worker/supervisor cleanup path.

```powershell
idf.py -C v2/qualification/wasm-worker -B build/m6-worker-xiao `
  -D IDF_TARGET=esp32c6 build
python v2/qualification/wasm-worker/run_worker.py --port COM13 `
  --chip esp32c6 --board seeed-xiao-esp32c6-chip-antenna `
  --build build/m6-worker-xiao --report build/m6-worker-xiao-trial.json
```

Use matching `esp32`/HUZZAH32 and `esp32s3`/M5Dial builds for the other families.
ESP32 defaults automatically enable single-core byte-accessible IRAM. The
runner writes app/OTA selection, records artifact/source hashes and leaves the
test firmware installed. It does not switch PC networks. Collect all three
reports with `v2/tools/wasm/collect_worker_evidence.py --output <json>`.

The [recorded native lifecycle evidence](../../../docs/v2/evidence/wasm/2026-10-07-native-worker-lifecycle.json)
is an isolated component trial. Production radio/LED/settings coexistence,
global or cold heap exhaustion, fragmentation, individual internal engine
allocation failures and longer soaks require additional qualification.

## Production guest-memory fixtures

`generate_fixtures.py` uses host-only `wasmtime==36.0.0` to generate
`main/fixtures.hpp`. Wasmtime is not a firmware dependency.

The production HIL driver, `v2/tools/control/blip_wasm_hil.py`, combines these
guest-memory fixtures with the existing service-policy fixtures. Memory cases
cover integer and floating-point loads/stores, unaligned accesses, arithmetic,
NaN/signed-zero bits, bulk operations, bounds, growth and reload zeroing. They
exercise the separate ESP32 linear arena and the contiguous C6/S3 engine pool.

Run serial qualification before traffic qualification. For a PC network switch,
use `blip_wasm_network_hil.ps1 -CoexistenceOnly` after the serial run. Separating
the trials keeps slow UART checks out of the guarded network window; the
independent 45-second recovery remains armed throughout the traffic trial.

`--skip-overflow` explicitly records omission of two large UART-burst checks.
The profile collector permits that gap only for the documented M5StickC serial
issue; the checks remain required on the other six profiles. Heap measurement
permits one recorded stabilization window, then requires a full non-growing
window. Neither exception establishes full Gate D acceptance.

The historical service project tests the runtime-neutral service lifecycle;
the native application above additionally exercises actual SDK task ownership.
