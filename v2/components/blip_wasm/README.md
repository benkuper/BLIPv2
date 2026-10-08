# blip_wasm

Runtime-neutral lifecycle service and the selected WAMR adapter. Component
providers depend on `capability.hpp`, which exposes no engine
headers, handles or allocator. The platform supplies pool/module buffers and
owns the worker. It must explicitly stop the service before destroying them.

[ADR-0008](../../../docs/v2/adr/0008-wasm-runtime-selection.md) selects the metered
WAMR fast interpreter after the three-family hardware comparison. The
[benchmark](../../qualification/wasm/README.md) is a separate qualification
project; its adapter is not the production service. The separate
[service qualification](../../qualification/wasm-service/README.md) builds this
implementation on all three processor families. The opt-in production worker
has Ball C6 and selected ESP32 coexistence evidence. The
[profile memory follow-up](../../../docs/v2/work-packages/6.3-wasm-profile-memory.md)
records seven builds and their declared hardware checks, with two large UART
burst checks omitted on the M5StickC after damaged replies. The
[production provider checkpoint](../../../docs/v2/work-packages/6.5-production-providers.md)
qualifies LED/fleet imports, worker reservations and three-family lifecycle/fault
tests. The serial issue, script controls/SDK and full Gate D remain open.

## Current contract

- `start → ready → load → loaded → call`; traps/cancellation transition to
  `faulted`, which requires reload. `unload` and `stop` release engine state.
- Requests use bounded export names, at most eight numeric arguments/results,
  copied module bytes and owned 128-byte diagnostics. Value transport preserves
  i32/i64/f32/f64 bits, including NaNs and signed zero.
- Lifecycle, signatures, calls and snapshots are worker-confined. The supervisor
  may call `Runtime::request_cancel()` concurrently. The adapter protects the
  active instance lifetime during cancellation; it never holds that guard across
  guest execution. Cancellation outside an active call is a no-op.
- The default WAMR profile permits at most one 64 KiB linear memory, a 4 KiB
  Wasm stack and 16 KiB module input. The caller's fixed pool also bounds decoded
  code/tables/engine allocations. Unsupported limits fail explicitly.
- WAMR global ownership is exclusive. Imports resolve only through immutable,
  component-owned capability descriptors. WASI, AOT/JIT and guest threads
  are disabled. The internal thread manager enables synchronized termination on
  loop backedges, without exposing guest threading.
- Load cannot run guest code: start sections and automatic constructor exports
  are rejected. Initialization must use an explicit budgeted export.
- Calls enforce instruction fuel. Cancellation tokens are admission checks;
  active token/deadline observation belongs to the platform supervisor. Nonzero
  runtime-level deadlines are refused. `EspWasmComponent` owns the wall-clock
  supervisor and passes fuel/admission cancellation into the runtime. It rejects
  late results; completion can be delayed by higher-priority SDK/cache work.

Production selects it with `-D BLIP_ENABLE_WASM=ON`; the default is off. Its
ESP-IDF CMake recipe requires `BLIP_WASM_DEPS` pointing to the pinned, unmodified
WAMR checkout (default `build/wasm-deps`). Core host tests
exercise the same service against interchangeable fake engines; they do not
compile WAMR or claim interpreter validation coverage.

## Production worker

[ADR-0010](../../../docs/v2/adr/0010-wasm-production-worker.md) declares the
priority-2 worker (8 KiB native stack) and priority-6 supervisor (4 KiB stack).
The lifecycle owner reserves 80 KiB for the engine and guest memory before
network startup. Upload storage is allocated on the worker to the accepted
module size, bounded by 16 KiB, after unloading/detaching previous storage.
[ADR-0017](../../../docs/v2/adr/0017-wasm-live-controls-upload-storage.md) records
the ownership and heap constraints. On the original ESP32, [ADR-0011](../../../docs/v2/adr/0011-esp32-wasm-linear-arena.md)
places the 64 KiB guest page in byte-accessible IRAM and keeps the 16 KiB engine
pool, module buffer and stacks in DRAM. That profile requires single-core mode;
C6/S3 retain the contiguous aligned pool. The optional runtime-neutral borrowed
arena is explicit and unsupported backends must reject it. For separate arenas,
peak usage is the conservative sum of their individual high-water marks.
Requests and results are copied into an eight-request queue and
sixteen-completion ring; overload rejects new work. IDs/epochs never repeat.

`upload_begin(bytes, crc32)`, contiguous `upload_chunk(offset, hex)` chunks of
up to 64 bytes, and `upload_commit` load a volatile module. Beginning an upload
unloads the previous module before reusing its retained bytes. CRC validation
is an integrity check. Decoded code/tables/memory must also fit the pool; an
admissible 16 KiB input is not guaranteed to load successfully.

`call0` and `call_i32` return asynchronous request IDs. `completion(id)` returns
ready/error/result-count/elapsed-us; `result(id,index)` returns type/low32/high32
numeric bits. Expired completions return an explicit error. The typed C++ entry
point supports all numeric signatures. `cancel_all` invalidates active, queued
and staged upload work; stale module generations cannot execute after reload.

Instruction/deadline parameters are captured per call. Defaults are 10,000
instructions and 10 ms; maxima are 1,000,000 and 50 ms. The WASM SDK profile uses
1 ms ticks, bounds Wi-Fi buffers and places optional Wi-Fi fast paths in flash.
Use a fresh build directory when enabling this SDK profile: defaults do not
replace values in an existing `sdkconfig`. The recipe refuses other tick rates.
Production LED/fleet providers expose their own versioned imports. Native
callbacks use immediate bounded admission/queries and checked guest-memory
copies. Their task-time budgets require 64-bit ESP timer runtime statistics;
the supervisor separately bounds the whole guest call in wall-clock time.
The control path never invokes guest code inline. Stop disables admission,
cancels and joins the worker. Routine stop/suspend retains the 80 KiB pool and
current upload buffer; permanent retirement releases them after quiescence.

The HIL driver is `v2/tools/control/blip_wasm_hil.py`. Optional `--ip` traffic
requires an already reachable board. `blip_wasm_network_hil.ps1` temporarily
joins a saved network using an independent, verified recovery process and a
temporary profile clone, then verifies the original Internet connection.
Use `-CoexistenceOnly` after separate serial qualification to keep slow UART
checks out of the independent recovery window.

## Script control declarations

`script_manifest.hpp` decodes an optional `blip.controls.v1` custom section into
owned, relocatable metadata: at most 16 parameters/actions/events, four fields
per action/event, and a 2 KiB text arena. Descriptor projections use the existing
core types. The complete format and loading/publication requirements are in
[ADR-0015](../../../docs/v2/adr/0015-wasm-script-control-declarations.md).

Production loading prepares declarations before engine load and publishes them
after callback/arena validation. Leased schema reads and owned parameter values
use the registry's dynamic hooks. Copied typed actions run on the supervised
worker with the existing ordered request IDs/completions; unload/replacement
closes admission immediately. Fault, unload and stop retire the schema. The
store and message scratch live outside the worker stack. The production HIL
driver is `v2/tools/control/blip_script_controls_hil.py`.

Guest parameter/event imports, transport generation tokens, lossless external
i64 support, automatic web schema refresh and SDK bindings remain pending.
The independent
`v2/qualification/wasm-controls/` project runs the same parser cases on all three
chip families without an interpreter or production services.
