# Checked UTF-8 qualification

The native ESP-IDF application runs the actual runtime-neutral service and
pinned WAMR adapter with production memory placement, a 4 KiB Wasm stack and an
8 KiB native worker stack. Guest functions return unsigned pointer/length
pairs. Tests read exact UTF-8 bytes and write them back into the same linear
memory, then use guest code to verify the host writes.

Cases cover empty strings, a string at the final memory byte without a
terminator, embedded NUL, multibyte characters, the 256-byte limit, too-long
inputs, malformed/truncated/overlong/surrogate/out-of-range UTF-8, short output
buffers, invalid/wrapped memory ranges, writes crossing the final byte, stale
module generations, reload initialization, absent memory and zero-to-one-page
growth. One hundred reload/copy/unload cycles must leave SDK heap and unloaded
pool use unchanged. ESP32 exercises byte-accessible IRAM and unaligned copies.

The generator uses host-only `wasmtime==36.0.0`; the generated fixtures are
tracked. The target has no Wasmtime dependency. It reuses the native worker
project's sdkconfig defaults and the production ESP32 arena defaults.

```powershell
idf.py -C v2/qualification/wasm-strings -B build/m6-strings-xiao `
  -D IDF_TARGET=esp32c6 build
python v2/qualification/wasm-strings/run_strings.py --port COM13 `
  --chip esp32c6 --board seeed-xiao-esp32c6-chip-antenna `
  --build build/m6-strings-xiao --report build/m6-strings-xiao-trial.json
```

Use `esp32`/HUZZAH32 and `esp32s3`/M5Dial for the other family builds. The runner
records artifact/source/fixture hashes, chip/MAC identity and runtime pin. It
writes app/OTA selection only, leaves the testing image installed and keeps the
PC network unchanged. The collector,
`v2/tools/wasm/collect_string_evidence.py --output <json>`, requires all three
native trials, seven production compatibility builds and the host test result.

See [work package 6.4](../../../docs/v2/work-packages/6.4-wasm-checked-utf8.md)
for measurements. These are isolated string API tests; provider bindings,
generated SDK and production coexistence remain subsequent work.
