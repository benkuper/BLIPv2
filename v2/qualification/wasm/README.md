# WASM runtime benchmark

Milestone 6.1 qualification firmware, separate from the production components.
Compare the same reviewed Wasm bytecode on ESP32, ESP32-S3 and ESP32-C6, with a
native baseline and pinned, unmodified released engines:

| Runtime | Release | Commit |
| --- | --- | --- |
| WAMR | 2.4.5 | `25bd7eb63e828e4bd242cc9b38d260b4b31c6605` |
| wasm3 | 0.9.0 | `0cd38327f0c721e75172f4f1eeb55854dc0517af` |

## Reproduce

Use the repository's ESP-IDF 6.0.2 environment. From the repository root:

```powershell
python v2/qualification/wasm/prepare_dependencies.py
& v2/qualification/wasm/build_matrix.ps1 -Chip esp32c6
& v2/qualification/wasm/build_matrix.ps1 -Chip esp32s3
& v2/qualification/wasm/build_matrix.ps1 -Chip esp32
```

The script reuses one SDK build per chip, then archives each engine's app, ELF,
map, config, OTA selection and machine-readable size data under
`build/m6-wasm-images/<chip>/<engine>`. Dependencies stay in ignored build output;
the CMake recipe rejects a wrong revision or modified tracked dependency files.

Run one connected board at a time with its **existing production restore build**:

```powershell
python v2/qualification/wasm/run_benchmark.py `
  --port COM13 --chip esp32c6 --board seeed-xiao-esp32c6-chip-antenna `
  --build build/m6-wasm-images/esp32c6/baseline `
  --build build/m6-wasm-images/esp32c6/wamr `
  --build build/m6-wasm-images/esp32c6/wasm3 `
  --restore build/m8-espnow-xiao --report build/m6-wasm-xiao.json
```

The runner writes only OTA selection at `0x18000` and the first application at
`0x20000`. It restores BLIP in `finally`, then verifies a serial control reply.
It never changes the PC's Wi-Fi. Do not use it on a board with a different
partition layout or on a board requiring a different flash/reset procedure.
If the process or computer is terminated externally, rerun the restore flash;
`finally` cannot survive process termination.

## What is measured

The fixture exports integer arithmetic, soft/hardware floating-point arithmetic,
256-pixel RGB generation with linear-memory stores, and a bounded host call.
Each workload has one warmup and 21 measured samples; report min/median/max and
validate every result. Native host calls execute a real non-inlined call and
native pixel generation performs observable stores to a common scratch buffer.
The entire guest memory must initially contain zero bytes. Noop is measured in
batches of 100 calls. Timing includes
the engine call wrapper but excludes serial output and inter-sample delays.

Fault trials cover unreachable, out-of-bounds load, divide by zero, recursion
overflow, rejected memory growth and an infinite loop. After every trap, a valid
call must succeed. The infinite-loop trial requests 10,000 WAMR fast-interpreter
instructions. The supervisor runs independently at higher priority and resets
after 100 ms if execution does not return. A reset is reported as failure to
isolate that script, not as successful runtime cancellation. RTC no-init memory
prevents the trial from repeating after that reset.

All variants use a 32 KiB native pthread stack and an 8 KiB Wasm stack. C6 runs at
160 MHz; Xtensa chips run at 240 MHz. No radios, LED drivers, storage or production
tasks are active. Their coexistence and deadline behavior belong to Gate D.

WAMR uses its fast interpreter with instruction metering, software bounds checks,
no AOT/JIT/WASI/guest threads, and a 128 KiB boot-allocated pool. The local port
adapter routes linear-memory mapping requests into that pool and rejects
executable or larger-than-one-page mappings. Its native stack boundary is set
once, with a 2 KiB cleanup guard, to avoid the upstream port scanning the entire
task stack on every invocation. The upstream filesystem/socket/mapping/clock
files are excluded; neither runtime exposes these APIs to the guest.

wasm3 uses ordinary SDK heap allocation, a one-page memory cap, its module
validator, a 256-value function-height limit and an 8 KiB native execution-stack
limit. Its fixed bump allocator is unsuitable for demonstrating reusable script
instances, so it is not enabled. Compare actual heap deltas with WAMR's **used**
pool bytes, and report WAMR's reserved pool separately. Pool metrics of zero in
other variants mean unavailable, not zero memory cost. Heap minima are whole-task
measurements, not exact engine allocator traces.

On Xtensa, wasm3's `M3_HAS_TAIL_CALL=0` option disables a `musttail` attribute
which GCC 15 advertises but cannot emit for the windowed ABI. This uses the
upstream fallback rather than modifying the runtime source. Runtime trials,
including stack exhaustion, must still pass with that configuration.

`fixtures/workloads.wat` is the reviewable input. The generated header is checked
in so firmware builds need no host Wasm compiler. Regeneration requires the
host-only `wasmtime==36.0.0` package:

```powershell
python -m pip install wasmtime==36.0.0
python v2/qualification/wasm/generate_fixtures.py
```

Reports identify the bytecode, image and harness SHA-256. Keep build-size and
runtime evidence together when interpreting a result. These small representative
workloads do not establish compatibility or speed for every possible script.
