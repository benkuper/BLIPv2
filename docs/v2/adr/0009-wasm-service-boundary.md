# ADR-0009: WASM service ownership and cancellation boundary

- Status: Accepted for the service/adapter boundary; production scheduling and Gate D open
- Date: 2026-10-07
- Related: [ADR-0008](0008-wasm-runtime-selection.md), [ADR-0003](0003-threading-and-scheduling.md)

## Decision

Components use the runtime-neutral `Service`/`Runtime` interfaces. The engine
implementation alone owns WAMR handles. Module input is copied into caller-owned
fixed scratch, retained through unload. The caller owns the engine pool and
must explicitly stop on the worker before releasing either buffer or runtime.
No filesystem, task creation, registry mutation or render callback is hidden
inside the portable service.

Preserve numeric WASM values as typed raw bits rather than converting through
the registry's user-facing scalar type. Check export names, signatures, arity,
output capacity, type and 32-bit value width before invocation. A trapped or
cancelled instance becomes faulted and cannot run again until reload. Copy fault
text into bounded owned storage; `core::Error` only carries stable detail tokens.

The selected engine is globally exclusive and uses the supplied fixed pool for
decoded code, instance state, VM stack and linear memory. Pool exhaustion is a
load failure, not an unbounded SDK heap fallback. The initial profile has one
64 KiB memory, 4 KiB VM stack and at most 16 KiB module bytes. Unsupported limits
are refused. Production boot RAM/profile compatibility is a separate gate.

Reject WASM start sections before loading, and reject WAMR's automatic
`__post_instantiate`, `__wasm_call_ctors` and `_initialize` exports before
instantiation. These paths can execute with unlimited fuel inside the engine's
loader/instantiator. Guest initialization instead uses an explicitly budgeted
export. No imports are accepted before component-owned providers exist.

## Cancellation and scheduling

Keep WAMR's internal thread manager enabled so asynchronous termination sets
synchronized suspend flags checked on loop backedges. Keep guest pthread/WASI
threads and shared memory disabled. This extends the 6.1 benchmark recipe for
host cancellation without enabling guest threads or changing its selection.
The adapter's short mutex protects active-call publication, cancellation and
instance teardown, but is not held across guest execution.

All other service/adapter operations are worker-confined. A supervisor may call
`request_cancel`; shutdown requires the supervisor to be quiescent before the
adapter itself is destroyed. Requests outside an active call are no-ops.
Instruction fuel is enforced per call. An atomic token is checked at admission;
the platform must observe changes and issue active cancellation. Until that
platform supervisor exists, a nonzero deadline is rejected explicitly.

The standalone service qualifier declares a 16 KiB native pthread worker at
priority 3 and a priority-5 supervisor. This does not authorize running a guest
inline on the cooperative control/render scheduler. The production queue,
deadline supervisor, resource/descriptor costs and real firmware coexistence
tests remain Milestone 6.3 implementation work.

## Evidence boundary

Host tests replace the backend without modifying the service client and verify
ownership, signatures, lifecycle and fault policy. The
[service qualifier](../../../v2/qualification/wasm-service/README.md) links the
actual adapter and exercises three chip families, traps, cancellation and
repeated lifecycle. It contains no production LED/radio/settings tasks. Neither
test suite closes Gate D or the seven-board production compatibility gate.
