# Portable component-owned WASM provider qualification

This isolated app registers two synthetic components through the real core
registry and exercises the portable capability catalog against actual WAMR
guest memory. It does **not** install guest-to-native imports or run production
radio/LED/settings tasks. The `native_imports_exercised` result remains false.
The checked-string fixture is reused without modification from `wasm-strings`.

In a PowerShell shell with ESP-IDF 6.0.2 exported and the accepted unmodified
WAMR checkout at `build/wasm-deps/wamr`:

```powershell
idf.py -C v2/qualification/wasm-capabilities -B build/m6-capabilities-xiao -D IDF_TARGET=esp32c6 build
python v2/qualification/wasm-capabilities/run_capabilities.py --port COM13 --chip esp32c6 --board seeed-xiao-esp32c6-chip-antenna --build build/m6-capabilities-xiao --report build/m6-capabilities-xiao-trial.json
```

The runner flashes app/OTA-selection sectors, records physical identity and
artifact/source hashes, captures completion and checks heap/stack limits. It
leaves testing firmware installed and does not change the PC network. Verify
the current port before flashing. The project also accepts `esp32` and
`esp32s3`; this checkpoint's native evidence is specifically XIAO C6.

After rebuilding the existing seven production qualification directories and
running all host suites, collect source-bound evidence with the ESP-IDF Python:

```powershell
python v2/tools/wasm/collect_capability_evidence.py --idf D:/Projects/Dev/.tools/esp-idf-v6.0.2 --output docs/v2/evidence/wasm/2026-10-08-component-provider-contract.json
```

Required logs are `build/m6-capabilities-host-{red,ctest}.log`,
`build/m6-capabilities-xiao-build.log`, and
`build/m6-capabilities-production-<profile>-build.log`. The collector checks
runtime/board identity, native source/artifact hashes, host completion, target
and feature selections, linker scope, partition fit and official linked sizes.
It distinguishes current app deltas from static RAM deltas against the earlier
production memory matrix. Historical qualification evidence stays unchanged.
