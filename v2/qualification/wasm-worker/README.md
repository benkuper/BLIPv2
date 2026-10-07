# Production worker qualification fixtures

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

This directory currently supplies memory fixtures. Native component task
start/stop/restart and allocation-failure qualification are still open; the
service lifecycle project does not establish those native task properties.
