# Guest-to-native WASM import qualification

This isolated app runs real guest calls through the WAMR raw native bridge into
two synthetic component-owned providers. It uses the production fixed engine
budget, module buffer, 4 KiB VM stack and 8 KiB native worker. It does not start
production radio, LED or settings tasks.

The fixture covers eleven registered functions, all numeric widths and raw
NaN/sign bits, eight mixed/wide arguments, void returns, checked UTF-8 copies,
failure/result validation, recursive dispatch, unavailable and suspended owners,
fuel exhaustion, callback overruns and cooperative native cancellation. Fifteen
negative modules cover undeclared/versioned imports, underscore aliases, embedded
NUL names, wrong signatures, non-function imports, constructors and excess
imports. A separate fixture accepts 32 imported function slots; this does not
saturate the eight-provider/32-function registration catalog.

Reload/unload runs 100 times with stable warmed SDK heap and registration-pool
usage. Twenty engine restarts check registration teardown. A link wrapper injects
failure at the first and second namespace registration, then verifies cleanup
and retry. Only this qualification ELF links the wrapper. The pinned runtime
checkout is never modified.

## Reproduce

Use an ESP-IDF 6.0.2 PowerShell environment and the unmodified WAMR checkout at
`build/wasm-deps/wamr`. Fixture regeneration alone needs host-only
`wasmtime==36.0.0`; firmware execution does not use Wasmtime.

```powershell
python v2/qualification/wasm-imports/generate_fixtures.py
idf.py -C v2/qualification/wasm-imports -B build/m6-imports-xiao -DIDF_TARGET=esp32c6 build
python v2/qualification/wasm-imports/run_imports.py --port COM13 --chip esp32c6 --board seeed-xiao-esp32c6-chip-antenna --build build/m6-imports-xiao --report build/m6-imports-xiao-trial.json
```

Repeat with the current ports for HUZZAH32/`esp32` (`m6-imports-huzzah32`)
and M5Dial/`esp32s3` (`m6-imports-m5dial`). Verify physical identity and the current
port before flashing. The runner writes app and OTA-selection sectors, captures
source/artifact hashes, validates completion, and leaves test firmware installed.
It never changes the PC's network. ESP32 uses the accepted 16 KiB DRAM engine
pool plus 64 KiB IRAM linear arena with unicore/IRAM byte access enabled.

After all 24 host suites and seven production compatibility builds pass:

```powershell
python v2/tools/wasm/collect_import_evidence.py --idf D:/Projects/Dev/.tools/esp-idf-v6.0.2 --output docs/v2/evidence/wasm/2026-10-08-native-import-bridge.json
```

The collector requires `build/m6-imports-host-{red,ctest}.log`,
`build/m6-imports-policy-host-red.log`, each native build log/trial, and
`build/m6-imports-production-<profile>-build.log` from the existing
`build/m6-production-<profile>` directories. It checks runtime/SDK/board identity,
source/artifact hashes, linked bridge and qualifier-wrapper scope, feature
selections, official linked sizes and partition fit. Historical evidence stays
pinned to its original sources and artifacts.

## Limits

Native callbacks are trusted, bounded code. Elapsed-time checks fault a script
after a callback returns; they cannot preempt a hung native callback or undo its
effects. Cancellation during a callback is cooperative. The intentionally slow
callback exceeds its 2,000 us declaration to verify this fault path.

Production provider exposure, lifecycle ordering and coexistence remain separate
6.5 work. These production compatibility images still have no configured
catalog and refuse nonempty imports. Full Gate D, stress, allocator exhaustion,
fragmentation and long soaks are not established by this isolated app.
