## Codex implementation plan

This plan is anchored to BLIP [`main` at e567eeb](https://github.com/Golden-Geek/BLIP/commit/e567eeb5f20ba022595fd5b89a77fb17ba59ca94). Development should happen beside the existing firmware until the new version reaches parity—no big-bang replacement.

### Operating assumptions

- Create a `v2` development branch.
- Keep the current Arduino firmware operational and unchanged except for fixture/export tools.
- Build V2 in `v2/` using pure ESP-IDF 6.0.x and C++20.
- Initial targets: ESP32, ESP32-S3, ESP32-C6.
- No exceptions or RTTI in embedded code.
- Prefer fixed-capacity buffers and explicit ownership.
- Preserve useful V1 compatibility, but not V1’s internal architecture.
- One independently reviewable subsystem per PR.
- Do not move or delete V1 until the cutover gate is explicitly approved.

## Repository layout

Codex should establish:

PathResponsibility`v2/firmware/`ESP-IDF application`v2/components/blip_core/`Lifecycle, registry, scheduler, errors`v2/components/blip_resources/`Pins, buses, DMA, memory and radio arbitration`v2/components/blip_storage/`NVS, filesystems, settings and migrations`v2/components/blip_transport/`Serial, Wi-Fi, BLE, ESP-NOW`v2/components/blip_oscquery/`OSC and OSCQuery server`v2/components/blip_led/`Pixel model, compositor, drivers and power limiting`v2/components/blip_wasm/`Runtime abstraction and capability providers`v2/components/blip_fleet/`Clock synchronization and autonomous coordination`v2/boards/`Board manifests and default pin assignments`v2/profiles/`Feature/build profiles`v2/web/`Schema-driven device UI and installer`v2/tools/kitchen/`Firmware configuration and build tooling`v2/tests/fixtures/v1/`Legacy settings, packets, OSCQuery and playback data`docs/v2/adr/`Architecture decision records

## Non-negotiable architectural rules

Codex must enforce these in every implementation PR:

1. The component registry is the single source of truth for:

- OSCQuery hierarchy
- Web controls
- Settings
- WASM functions and parameters
- Runtime diagnostics
- Generated documentation

2. The LED render/output path must never perform:

- Heap allocation
- Filesystem operations
- Logging
- Network operations
- Blocking component callbacks

3. Components communicate through typed services, bounded queues and events. They must not reach into unrelated global singletons.
4. Every optional feature must declare:

- Flash and RAM cost
- Required peripherals
- Pins and buses
- Radio dependencies
- Incompatible features
- Whether it can be disabled live or requires reboot

5. Hardware-specific behavior stays behind capability interfaces. Application components must not directly depend on target-specific ESP-IDF drivers.
6. Every persisted structure and wire protocol is versioned from its first commit.

## Implementation roadmap

Each numbered item should normally become one PR. Large component-parity items can become an epic containing one PR per component.

### Milestone 0 — Preserve observable V1 behavior

PRWorkAcceptance0.1Document current OSC paths, settings, playback files, communication formats and component list.Public compatibility matrix checked into `docs/v2/`.0.2Capture representative V1 fixtures: settings, OSCQuery tree, serial messages, ESP-NOW packets and playback files.Fixtures can be consumed by host-side tests without hardware.0.3Write ADRs for component lifecycle, resource ownership, threading, memory policy, persistence and compatibility.No production V2 implementation begins before these contracts exist.This does not require faithfully reproducing V1 bugs or undocumented internal coupling.

### Milestone 1 — Buildable foundation

PRWorkAcceptance1.1Create the ESP-IDF project, dependency lock, formatting rules and CI matrix.Minimal firmware builds for ESP32, S3 and C6.1.2Implement component descriptors, registry and lifecycle: construct, validate, start, suspend, resume, stop.Host tests cover ordering, duplicate IDs, dependency cycles and failure cleanup.1.3Implement typed parameters, actions, events and metadata.A synthetic component produces a complete machine-readable descriptor.1.4Add a resource broker for GPIO, RMT, SPI, I2C, UART, timers, DMA, PSRAM and radio resources.Conflicting board/profile configurations fail at build time where possible and boot time otherwise.1.5Add the scheduler and bounded event bus.Queue overflow is visible and deterministic; real-time work has no unbounded execution.**Gate A:** the same core and component model boots on all three chip families.

### Milestone 2 — Settings, diagnostics and basic control

PRWorkAcceptance2.1Add versioned per-component settings backed by NVS.Interrupted saves retain either the previous or new valid state.2.2Add LittleFS and optional SD storage services with atomic replacement.Files survive forced resets during update tests.2.3Add settings migration and V1 settings import.Golden V1 fixtures import into the expected V2 component values.2.4Implement structured logging, runtime log levels, metrics, reset-cause reporting, coredumps and safe mode.Boot loops enter a diagnosable recovery mode.2.5Implement the common transport envelope and Serial/USB transport.Parameters and actions can be controlled through a host test utility.2.6Implement Wi-Fi station/AP management and serial/SoftAP provisioning.A blank device can be configured without reflashing.2.7Implement OSC UDP and OSCQuery HTTP/WebSocket discovery.The hierarchy is generated exclusively from the registry and passes protocol fixtures.

### Milestone 3 — First complete vertical slice

PRWorkAcceptance3.1Build the schema-driven web application shell.Adding a test component requires no hand-written control panel.3.2Add filesystem management and separately updatable web assets.UI assets can be replaced without reflashing the application partition.3.3Implement A/B OTA, rollback, image/profile validation and boot confirmation.Failed and interrupted updates recover automatically.3.4Add the first LED backend—single-strip WS2812/SK6812 through native RMT.Web and OSC control a real strip on each reference target.3.5Add browser installation using esptool-js/Launchpad-compatible manifests.A factory device can be flashed, provisioned, opened and updated from a browser.**Gate B:** browser install → Wi-Fi setup → OSCQuery discovery → LED control → settings save → OTA rollback works end to end.

### Milestone 4 — Production LED engine

PRWorkAcceptance4.1Implement the pixel surface and output-driver ABI.Drivers advertise pixel format, timing, maximum length, buffering and synchronization capabilities.4.2Implement the compositor and `stream`, `playback`, `script`, `system` layers.Layer priority, opacity and blend behavior have deterministic host tests.4.3Add linear-light color transforms, channel ordering, calibration and correction.Known color vectors produce golden outputs.4.4Add asynchronous frame submission, buffer pooling and deadline metrics.The render task performs zero allocations after startup.4.5Add native clocked-strip support: APA102/SK9822, HD108 and compatible protocols.Protocol analyzer or loopback fixtures verify frame layout and timing.4.6Add optional parallel output and an optional FastLED compatibility backend.Native and compatibility backends use the same output ABI.4.7Implement streaming and playback, including legacy import.Network streaming does not block LED output; corrupted playback files fail safely.4.8Add Art-Net/DMX compatibility, followed by optional E1.31/DDP components.Each is independently removable from minimal builds.**Gate C:** sustained Wi-Fi traffic cannot corrupt LED timing, exhaust queues or cause monotonic heap loss.

### Milestone 5 — Battery, power and radios

PRWorkAcceptance5.1Implement battery sensing, filtered state-of-charge inputs and board-specific calibration.Calibration and low-battery states are exposed through the registry.5.2Implement LED current estimation, configurable power budgets, smoothing and hard safety limits.Output never exceeds its configured calculated budget.5.3Add ESP-IDF power-management locks, light/deep sleep coordination and wake sources.Each board profile records measured idle and sleep behavior.5.4Implement `RadioManager` profiles and explicit memory/resource policies.UI clearly distinguishes live suspension from reboot-required memory reclamation.5.5Add NimBLE serial/service transport. Add Classic Bluetooth only for original ESP32 profiles.BLE can be excluded entirely and suspended when unused.5.6Implement ESP-NOW V1/V2 negotiation, fragmentation, sequence numbers, acknowledgements and duplicate handling.Tests cover V1↔V1, V2↔V2 and mixed-fleet behavior.5.7Implement fleet clock synchronization, leader election and scheduled cues.Nodes recover from leader loss without blocking normal computer control.

### Milestone 6 — WASM

PRWorkAcceptance6.1Create a target-representative wasm3/WAMR benchmark harness.ADR records code size, RAM, execution speed, fault handling and maintenance status.6.2Implement a runtime-neutral WASM service and select WAMR unless benchmarks disqualify it.Runtime can be replaced without modifying components.6.3Add bounded memory, stack, execution budget, cancellation and fault isolation.Infinite loops, traps and invalid pointers cannot stall LED or communication tasks.6.4Implement UTF-8 strings as checked pointer-and-length values.Tests cover empty, long, invalid and unterminated inputs.6.5Implement component-owned capability providers.Components register their own functions instead of editing a central function library.6.6Allow scripts to declare parameters, actions and events.Script-defined controls automatically appear in OSCQuery and the web UI.6.7Add generated script SDK bindings and the LED script layer.Example scripts compile and run against versioned capability manifests.**Gate D:** a faulty script is terminated and diagnosed while LED output, settings and networking remain operational.

### Milestone 7 — Current BLIP component parity

Port components in dependency order, normally one component per PR:

1. Generic GPIO and PWM
2. Button and DIP switch
3. Battery/power
4. IR input
5. Distance sensors
6. BNO055
7. BNO08x and M5 motion variants
8. PWM LED
9. Servo
10. Stepper
11. DC motor
12. Display
13. Wired DMX
14. RF24
15. Behaviour
16. Sequence
17. Flowtoys Connect
18. Existing LED FX interfaces, only where still useful
19. Dummy/test component
    Every parity PR must include:

- Board/resource declaration
- Component schema
- Settings migration if relevant
- OSCQuery and web exposure
- Host tests
- At least one hardware test record
- Flash/RAM impact for every enabled profile

### Milestone 8 — Firmware Kitchen

PRWorkAcceptance8.1Define board, feature and profile manifest schemas.Invalid combinations report understandable conflicts.8.2Generate `sdkconfig`, partition tables, component selection, pins and web schemas.Generated files are reproducible from a committed manifest.8.3Add a local CLI and reproducible containerized builder.Identical inputs produce identical application artifacts.8.4Build the web Kitchen for board selection, capabilities and configuration.It exports a manifest and installable firmware bundle.8.5Add CI build caching and a release artifact matrix.Minimal, standard, LED-heavy, sensor-heavy and full profiles build automatically.

### Milestone 9 — Hardening and cutover

- Fuzz settings, OSC, OSCQuery, file, ESP-NOW and WASM boundaries.
- Run reset, brownout, network-loss and corrupted-storage fault injection.
- Run multi-day LED/network/ESP-NOW soak tests.
- Record stack high-water marks and heap trends.
- Validate OTA interruption at each update stage.
- Test all canonical profiles on all supported targets.
- Publish board bring-up and component-authoring guides.
- Ship `v2.0.0-alpha`, then beta, before changing the repository default.
- Move or archive V1 only after explicit approval.

## Codex workflow for every PR

Codex should follow this loop:

1. Read the applicable ADRs and relevant V1 implementation.
2. State which public behavior is preserved, changed or intentionally removed.
3. Add or update tests before production code.
4. Implement only the current work package.
5. Build every affected profile for ESP32, ESP32-S3 and ESP32-C6.
6. Run host tests and applicable hardware-in-the-loop tests.
7. Report flash, static RAM, runtime heap and task-stack changes.
8. Update component manifests and documentation.
9. Open a focused PR with rollback and migration notes.
10. Do not begin a dependent PR until the previous contract is stable.
    Codex must pause for approval before changing public OSC paths, dropping a legacy file format, adopting a materially different dependency, or deleting/moving V1.
11. Commit and push after each Milestone

## Recommended first Codex assignment

Start with Milestone 0 only:

> Create the V2 planning foundation on a new `v2` branch. Preserve the existing firmware unchanged. Add `docs/v2`, architecture decisions, the complete V1 component/behavior inventory, compatibility fixtures, target profiles, measurable performance/stability gates, and a PR/issue dependency map. Do not implement production V2 firmware in this PR.
