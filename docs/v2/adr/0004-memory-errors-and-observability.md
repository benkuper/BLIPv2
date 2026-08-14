# ADR-0004: Memory, errors, and observability

- Status: Accepted
- Date: 2026-08-14
- Owners: BLIP V2 core

## Context

The firmware runs on constrained targets with different internal RAM/PSRAM and
radio costs. V1 uses dynamic strings/vectors and temporary allocations in control
and persistence paths, and signals many errors with booleans or logs. V2 disables
C++ exceptions and RTTI and needs reliable long-running LED/network behavior.

## Decision

Embedded V2 code compiles with exceptions and RTTI disabled. Fallible functions
return a typed `Result<T, Error>`-style value (or `Status` for no result). Errors
contain a stable domain/code, component ID, operation, bounded context fields,
and optional causal error. Human text is produced outside real-time context.

Memory policy:

- descriptors, queues, task stacks, and normal component capacity are fixed at
  build/startup;
- ownership uses values, spans/views with proven lifetime, move-only handles,
  and fixed/pool-backed buffers; raw owning pointers are forbidden;
- internal DMA-capable memory and PSRAM are distinct resource classes;
- allocation is permitted during construction/start in declared pools, but a
  component must handle exhaustion without partial registration;
- after render/output startup, that path performs zero allocations or frees;
- network/file inputs have explicit byte, nesting, collection, and time limits;
- no `new`, `malloc`, dynamic container growth, or formatting allocation is
  allowed in ISR or render/output context.

Panics/asserts are reserved for violated internal invariants where continuing
would be unsafe. External input, resource exhaustion, corrupt storage, and
unsupported configuration are normal typed errors and must not panic.

Observability includes structured logs with runtime levels, counters/gauges,
queue high-water/overflow, execution/deadline metrics, reset cause, coredump
identity, heap minima/largest block, task stack high-water marks, settings/migrate
status, and safe-mode reason. Logging is bounded and lossy under pressure; log
loss is counted. Secrets and full credential values are never logged.

Every optional feature reports build flash, static DRAM/IRAM, boot-time and
steady heap, pool sizes, task stacks, and the manifest digest used to measure it.

## Consequences

- Error paths are explicit and testable without exceptions.
- Fixed-capacity design can reject workloads previously accepted until failure;
  diagnostics must make the configured limit clear.
- Instrumentation and context fields consume bounded memory.
- Feature cost regressions become reviewable rather than anecdotal.

## Required tests

Pool exhaustion, maximum-size valid inputs, every parser bound, error propagation
with cleanup causes, secret redaction, log overflow, metric saturation behavior,
render allocation traps, heap trend, stack high-water, coredump/reset reporting,
and safe-mode entry/exit are required by the relevant work packages.

