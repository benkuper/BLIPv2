# UART burst diagnostic

This opt-in trace correlates raw bytes returned by pyserial with the complete
response frame immediately before `uart_write_bytes`. It uses the production
COBS/envelope decoders to validate the encoded reply and records its length,
CRC32, request ID and driver return count. A fixed 32-entry ring is dumped only
when a later normal `blip.bootstrap/probe_value` read arrives. No trace logging
occurs during the tested burst.

The CMake project hook attaches the wrapper only to the selected diagnostic
ELF. Production component sources and vendor code are unchanged. Build with all
the usual production options, plus the explicit hook:

```powershell
idf.py -C v2/firmware -B build/m6-m5stickc-uart-trace `
  -D IDF_TARGET=esp32 -D BLIP_BOARD_M5STICKC=ON `
  -D BLIP_ENABLE_WASM=ON -D BLIP_ENABLE_ESPNOW=ON `
  -D BLIP_ENABLE_FLEET=ON `
  -D CMAKE_PROJECT_INCLUDE=D:/Projects/Dev/BLIPv2/v2/qualification/serial-burst/inject.cmake build
python v2/qualification/serial-burst/run_burst.py --port COM12 `
  --board m5stack-m5stickc --build build/m6-m5stickc-uart-trace `
  --trace --flash --report build/m6-m5stickc-uart-trace-trial.json
```

The runner flashes only app/OTA selection through the ROM at 115200, leaves
the testing image installed and keeps the PC network unchanged. Without
`--flash` it expects the supplied image to be installed already. Without
`--trace` it records an ordinary production burst. Inputs must identify the
requested ESP32 board and enable WASM.

Each trial uploads the known spin fixture, sends twelve calls and cancellation,
records timeouts and raw malformed candidates, then requests the trace. Single
byte insertion is an offline diagnostic inference: a candidate must validate
both COBS and the original envelope CRC and match the pre-driver frame CRC to
localize the loss. Reconstructed frames never enter the control client. The
runner unloads the guest after each trial and on exit.

This is a diagnostic trial, not queue-overload acceptance. The twelve UART
requests can outlast the guest deadline, allowing more than eight admissions
as the worker advances. Trace decoding adds some CPU/stack use; a failure that
does not recur with instrumentation remains unresolved. A driver return count
means the bytes were accepted by the driver, not independently verified on the
physical wire. USB/serial paths still require separate observation.

The [recorded localization](../../../docs/v2/work-packages/6.3-m5stickc-uart-localization.md)
includes both an ordinary production burst and three instrumented trials.
`v2/tools/control/collect_serial_burst_evidence.py --output <json>` checks
the build/source hashes and matching deletions before saving the aggregate.
The raw invalid-candidate list also includes expected plaintext console output
between delimiters; only matched binary candidates establish missing replies.
