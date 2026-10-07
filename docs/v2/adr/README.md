# Architecture decision records

| ADR | Decision | Status |
| --- | --- | --- |
| [0001](0001-component-lifecycle-and-registry.md) | Component lifecycle and registry | Accepted |
| [0002](0002-resource-ownership.md) | Exclusive resource ownership and capability interfaces | Accepted |
| [0003](0003-threading-and-scheduling.md) | Task classes, scheduler, queues, and real-time boundary | Accepted |
| [0004](0004-memory-errors-and-observability.md) | Bounded memory, explicit errors, and observability | Accepted |
| [0005](0005-persistence-and-migrations.md) | Versioned transactional persistence and migrations | Accepted |
| [0006](0006-v1-compatibility-boundary.md) | V1 compatibility through explicit boundary adapters | Accepted |
| [0007](0007-led-transport-qualification.md) | Milestone 3 LED transport baseline | Accepted for M3 vertical slice |
| [0008](0008-wasm-runtime-selection.md) | Metered WAMR selection from three-family target benchmarks | Accepted for M6 runtime selection; Gate D open |

Accepted ADRs are immutable contracts. A material change is recorded by a new
ADR with `Supersedes`/`Superseded by` links.
