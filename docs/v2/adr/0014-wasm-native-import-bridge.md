# ADR-0014: Bounded guest-to-native capability bridge

- Status: Implemented; qualified on ESP32, ESP32-S3 and ESP32-C6
- Date: 2026-10-08
- Related: [ADR-0012](0012-wasm-checked-utf8.md), [ADR-0013](0013-wasm-component-capabilities.md)

## Engine boundary and ownership

`Runtime::configure_capabilities` borrows an immutable bound component catalog
before initialization, on the worker. Unsupported replacement backends refuse
a non-null catalog explicitly. The component catalog, providers and descriptors
outlive runtime shutdown; no interpreter headers or handles enter their API.
Shutdown clears the configured catalog, so a new initialization configures it
again.

WAMR owns one fixed registration table containing copied namespace/function
names, numeric signatures, mutable native symbols and stable attachments. Its
compile-time size is at most 4 KiB on supported 32-bit targets. It and WAMR's
registration nodes allocate exclusively from the existing engine pool; there
is no fallback to SDK heap. No table is allocated for an absent/empty catalog.
Symbols may be sorted by WAMR without invalidating copied names or attachments.

Initialization registers groups by exact namespace. A partial registration
failure unregisters the successful prefix, frees the table, destroys the engine
and releases global ownership. Normal shutdown unloads the instance first,
then unregisters groups and releases metadata before engine destruction.

## Explicit module policy

Before WAMR mutates module bytes or interns names, a bounded portable section
reader checks original import/export names, valid UTF-8, embedded NUL exclusion,
exact namespace/function membership and a maximum of 32 function imports.
Memory, table and global imports are refused. Start sections and automatic
constructor exports remain refused, preserving non-executing module admission.
Custom/data sections retain normal guest UTF-8 data, including U+0000.

This guard is needed because the pinned engine presents interned names through
C strings and accepts a leading-underscore native lookup alias. Neither name
truncation nor that alias is part of the BLIP contract. The reader is an
admission policy rather than a second complete WASM validator. WAMR validates
the binary and linkage; the adapter then explicitly checks every numeric import
signature and linked function before instantiating. Unconfigured production
images continue to refuse nonempty imports.

## Dispatch, cancellation and failures

Raw registration uses only `i/I/f/F` signatures, never engine pointer/string
conversions. The trampoline verifies the active worker invocation, current
execution environment/instance, runtime owner and attachment binding. Fixed
argument/result scratch carries raw numeric bits, including unsigned values,
NaNs and eight wide arguments. The portable catalog checks signature,
availability, cancellation, result types and recursive dispatch.

Callbacks receive only the current instance's checked memory interface through
`CallContext`. They cannot retain spans or context, call guest code, allocate,
perform I/O or wait unboundedly. The supervisor's `request_cancel` sets an atomic
callback token and terminates guest execution; cooperative providers observe
that token. Guest instruction fuel continues to bound loops around callbacks.

The bridge measures each callback's scheduled task execution time. ESP-IDF's
64-bit ESP timer runtime statistics exclude time spent running other tasks or
blocked; interrupt service and bounded measurement overhead remain included.
Yields flush the worker's unfinished accounting slice before and after each
callback. `vTaskGetInfo` reads only that task without allocation or stack scans.
The counter clock does not depend on CPU frequency. Builds require this accounting
configuration; there is no silent wall-time fallback.

Production BLE/radio preemption previously made short validation failures exceed
their 2 ms wall-time declaration. The callback budget now measures its own work;
the existing supervisor still enforces the whole guest call's absolute wall-time
deadline. This distinction does not permit providers to block or perform I/O.
A declared task-time overrun faults the script
after the callback returns. Native callbacks are trusted code; this does not
forcibly preempt a hung provider. Cancellation and overruns do not undo effects.
Provider failures become guest exceptions and stable error codes. Caller results
remain untouched with count zero. Bounded fault text contains namespace,
function and status, without guest argument text. Runtime snapshots expose
saturating native call/failure counts, last/maximum callback wall time and
last/maximum scheduled task execution time.

## Qualification and remaining composition

[Native evidence](../evidence/wasm/2026-10-08-native-import-bridge.json) records
real guest dispatch on the three families, malformed module/name/signature
rejection, raw numeric/text behavior, suspended owners, fuel/overrun/cancellation
faults, registration failure rollback, stable reload heap and engine teardown.
Seven production profiles compile and fit with the bridge linked.

The isolated qualifier uses synthetic owners and no production tasks. Actual
LED/fleet provider exposure, worker-before-provider shutdown ordering, production
coexistence, a saturated native catalog, allocator exhaustion, fragmentation,
stress and soaks remain separate work. The accepted portable contract and this
bridge alone do not close milestone 6.5 or Gate D.
