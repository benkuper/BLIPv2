# blip_wasm

Runtime-neutral lifecycle service and the selected WAMR adapter. Component
providers depend on `runtime.hpp` and `service.hpp`, which expose no engine
headers, handles or allocator. The platform supplies pool/module buffers and
owns the worker. It must explicitly stop the service before destroying them.

[ADR-0008](../../../docs/v2/adr/0008-wasm-runtime-selection.md) selects the metered
WAMR fast interpreter after the three-family hardware comparison. The
[benchmark](../../qualification/wasm/README.md) is a separate qualification
project; its adapter is not the production service. The separate
[service qualification](../../qualification/wasm-service/README.md) builds this
implementation on all three processor families. Production component/task
integration, provider/SDK work and Gate D remain open.

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
- WAMR global ownership is exclusive. Imports remain unavailable until the
  capability-provider boundary is implemented. WASI, AOT/JIT and guest threads
  are disabled. The internal thread manager enables synchronized termination on
  loop backedges, without exposing guest threading.
- Load cannot run guest code: start sections and automatic constructor exports
  are rejected. Initialization must use an explicit budgeted export.
- Calls enforce instruction fuel. Cancellation tokens are admission checks;
  active token/deadline observation belongs to the platform supervisor. Nonzero
  deadlines are currently refused because that production supervisor is not yet
  implemented. No wall-clock or real firmware task coexistence claim is made.

The component is not enabled in the production firmware entry point yet. Its
ESP-IDF CMake recipe requires `BLIP_WASM_DEPS` pointing to the pinned, unmodified
WAMR checkout. The standalone qualifier supplies that property. Core host tests
exercise the same service against interchangeable fake engines; they do not
compile WAMR or claim interpreter validation coverage.
