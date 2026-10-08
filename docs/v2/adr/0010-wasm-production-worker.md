# ADR-0010: Supervised WASM worker in production

- Status: Accepted for the Ball production slice; full qualification open
- Date: 2026-10-07
- Superseded for startup/upload reservation: [ADR-0017](0017-wasm-live-controls-upload-storage.md)
- Related: [ADR-0003](0003-threading-and-scheduling.md), [ADR-0009](0009-wasm-service-boundary.md)

## Decision

Declare a dedicated script worker and supervisor as an exception to the shared
task classes. Untrusted interpreter execution cannot run inline on control or
render jobs. Components still use the runtime-neutral boundary; the firmware
entry point selects the WAMR implementation.

| Task | Priority | Native stack | Affinity | Work |
| --- | ---: | ---: | --- | --- |
| Script worker | 2 | 8 KiB | none | upload, validation, load/unload, typed calls, completion |
| Script supervisor | 6 | 4 KiB | none | 1-tick deadline/epoch observation and active cancellation |

The worker borrows an 80 KiB engine pool and 16 KiB module scratch, allocated once
at the first component start in internal RAM. Routine stop/suspend retains these
fixed buffers: production radio/render allocations can fragment a freed module
buffer's space and prevent restart. `buffer_reserved` exposes the 96 KiB
reservation independently of the stopped engine's zero `pool_reserved`.
Cold-start failure rolls back new reservations after quiescence; a failed warm
start retains existing reservations. Permanent retirement calls
`release_reservation` after stop/join; destruction requires that quiescence and
releases the reservation. Live retirement is refused.
Allocation/initialization failure rejects startup. The 4 KiB Wasm stack is inside the engine pool. No module input, queue
payload or completion points into a transport/producer stack frame.

The standalone service qualifier used a 16 KiB native stack with at least
13,148 bytes remaining. Production starts with 8 KiB and must measure its own
headroom under control/load/trap/deadline tests. Future native providers require
renewed stack qualification.

The standalone qualifier reserved 96 KiB and measured 72,176 bytes peak engine
use. The production pool is smaller to preserve network/settings heap headroom
on the full Ball profile. The 16 KiB input limit is an admission limit; decoded
code, tables and memory must also fit the pool or loading fails explicitly.
The WASM-enabled SDK profile bounds Wi-Fi packet buffers, moves optional Wi-Fi
fast paths out of IRAM and uses 1 ms FreeRTOS ticks. Its throughput and deadline
behavior require coexistence qualification.

The request queue holds eight owned requests and rejects new work when full;
rejections are counted. Sixteen completions hold typed numeric results and error
codes. IDs never repeat within the component lifetime; exhausted IDs/epochs are
refused. Completion polling is bounded, and overwritten completion tokens return
an explicit error. Control actions return request IDs immediately. A completion
query and indexed raw-bit result query avoid long control-task waits and lossy
numeric conversions.

Module uploads must be contiguous and complete, with a matching CRC-32, before
load. Beginning an upload unloads the old instance before touching its retained
module bytes. Calls carry the observed module generation, preventing queued
calls from running on a replacement module. Cancellation increments a work
epoch; active and queued work from the previous epoch is cancelled. Pending
requests receive cancelled completions during stop.

## Deadline and lifecycle contract

Each queued call captures its instruction and wall-clock budgets. Defaults are
10,000 instructions and 10 ms; accepted limits are 1–1,000,000 instructions and
1–50 ms. The platform publishes an absolute monotonic deadline before invoking
the service. The runtime receives fuel and the admission token; the platform
supervisor observes the deadline/token and calls its thread-safe cancellation
method. It repeats cancellation while active to cover the admission/publication
race. It stays alive during stop until the worker is quiescent.

A late successful return is still a failed call; its results are discarded and
the module becomes non-runnable until reload. The supervisor's short guard is
not held across guest execution. Control admission/snapshot guards wait at most
2 ms. Worker/maintenance synchronization may block. Stop disables admission,
invalidates the epoch, cancels execution, waits at most 1 s for quiescence and
joins the worker before permitting permanent buffer retirement. Failure to quiesce retains resources
and reports an error, rather than freeing live interpreter memory.

The deadline starts cancellation; it is not a guarantee that a completion is
published at that exact time. Higher-priority SDK tasks and flash/cache work
can delay worker retirement. Evidence must report actual elapsed time alongside
the requested budget. Guest fuel remains a separate instruction bound.

The descriptor declares fixed startup RAM reservations and both native stacks.
The `static_ram_bytes` cost includes the pool/module buffers even though they
come from the startup heap; it is a fixed reservation estimate, not a linked
`.bss` measurement. Runtime metrics separately expose reserved/used/peak pool,
queue/completion counts, deadlines, failures and stack headroom. Profile builds
must establish actual linked cost and boot heap availability.

## Required evidence

Host validation covers bounded upload/CRC/lifecycle policy. Production HIL must
inject traps, invalid memory and tight loops while real LED output, serial and
network control, and settings commits continue. Queue overload, active/queued
cancellation, stale generations, recovery by reload, completion expiry and
repeated lifecycle must be qualified. One processor/profile is tested first;
seven board profiles are qualified after the worker behaves correctly. Gate D
remains open until all required profile/lifecycle evidence exists. No guest imports or blocking
native providers are enabled at this stage.
