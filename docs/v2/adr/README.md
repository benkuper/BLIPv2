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
| [0009](0009-wasm-service-boundary.md) | Runtime-neutral ownership, passive loading and host cancellation | Accepted for service boundary; production integration and Gate D open |
| [0010](0010-wasm-production-worker.md) | Dedicated worker, owned queue/completions and deadline supervisor | Ball production slice qualified; full Gate D open |
| [0011](0011-esp32-wasm-linear-arena.md) | ESP32 byte-accessible IRAM guest arena with DRAM engine/stacks | Seven production profile memory trials qualified; full Gate D open |
| [0012](0012-wasm-checked-utf8.md) | Checked unsigned pointer/length UTF-8 copies, bounds and generation | Host and three-family strings qualified; provider bindings follow in 6.5 |
| [0013](0013-wasm-component-capabilities.md) | Component-owned versioned provider descriptors, bounded catalog and checked callbacks | Portable contract qualified; native import bridge follows |
| [0014](0014-wasm-native-import-bridge.md) | Bounded WAMR guest-to-native bridge | Three chip families qualified |
| [0015](0015-wasm-script-control-declarations.md) | Owned custom-section declarations and leased registry projection | Declaration/interfaces qualified; live production controls pending |
| [0016](0016-wasm-owned-script-controls.md) | Owned values/queues, retirement and fixed guest callback buffer | Store/runtime query qualified; production wiring pending |
| [0017](0017-wasm-live-controls-upload-storage.md) | Production declaration publication, ordered script actions and upload-sized storage | Accepted for publication/actions; guest imports/events/web refresh remain open |

Accepted ADRs are immutable contracts. A material change is recorded by a new
ADR with `Supersedes`/`Superseded by` links.
