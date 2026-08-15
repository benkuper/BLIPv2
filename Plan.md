
## Codex implementation plan

This plan is anchored to BLIP [`main` at e567eeb](https://github.com/Golden-Geek/BLIP/commit/e567eeb5f20ba022595fd5b89a77fb17ba59ca94). Development should happen beside the existing firmware until the new version reaches parity—no big-bang replacement.

### Operating assumptions

* Create a `v2` development branch.
* Keep the current Arduino firmware operational and unchanged except for fixture/export tools.
* Build V2 in `v2/` using pure ESP-IDF 6.0.x and C++20.
* Initial targets: ESP32, ESP32-S3, ESP32-C6.
* No exceptions or RTTI in embedded code.
* Prefer fixed-capacity buffers and explicit ownership.
* Preserve useful V1 compatibility, but not V1’s internal architecture.
* One independently reviewable subsystem per PR.
* Do not move or delete V1 until the cutover gate is explicitly approved.

## Repository layout

Codex should establish:

| Path                              | Responsibility                                       |
| --------------------------------- | ---------------------------------------------------- |
| `v2/firmware/`                  | ESP-IDF application                                  |
| `v2/components/blip_core/`      | Lifecycle, registry, scheduler, errors               |
| `v2/components/blip_resources/` | Pins, buses, DMA, memory and radio arbitration       |
| `v2/components/blip_storage/`   | NVS, filesystems, settings and migrations            |
| `v2/components/blip_transport/` | Serial, Wi-Fi, BLE, ESP-NOW                          |
| `v2/components/blip_oscquery/`  | OSC and OSCQuery server                              |
| `v2/components/blip_led/`       | Pixel model, compositor, drivers and power limiting  |
| `v2/components/blip_wasm/`      | Runtime abstraction and capability providers         |
| `v2/components/blip_fleet/`     | Clock synchronization and autonomous coordination    |
| `v2/boards/`                    | Board manifests and default pin assignments          |
| `v2/profiles/`                  | Feature/build profiles                               |
| `v2/web/`                       | Schema-driven device UI and installer                |
| `v2/tools/kitchen/`             | Firmware configuration and build tooling             |
| `v2/tests/fixtures/v1/`         | Legacy settings, packets, OSCQuery and playback data |
| `docs/v2/adr/`                  | Architecture decision records                        |

## Non-negotiable architectural rules

Codex must enforce these in every implementation PR:

1. The component registry is the single source of truth for:

   * OSCQuery hierarchy
   * Web controls
   * Settings
   * WASM functions and parameters
   * Runtime diagnostics
   * Generated documentation
2. The LED render/output path must never perform:

   * Heap allocation
   * Filesystem operations
   * Logging
   * Network operations
   * Blocking component callbacks
3. Components communicate through typed services, bounded queues and events. They must not reach into unrelated global singletons.
4. Every optional feature must declare:

   * Flash and RAM cost
   * Required peripherals
   * Pins and buses
   * Radio dependencies
   * Incompatible features
   * Whether it can be disabled live or requires reboot
5. Hardware-specific behavior stays behind capability interfaces. Application components must not directly depend on target-specific ESP-IDF drivers.
6. Every persisted structure and wire protocol is versioned from its first commit.

## LED output transport recommendations

These recommendations are mandatory inputs to LED work. They are provisional
selection guidance until the qualification measurements below establish the
actual crossover points. A functional vertical slice using one peripheral must
not silently make that peripheral the production default.

### Architecture and selection rules

1. Keep pixel/protocol encoding independent from the hardware transport:

   * protocol encoders produce WS2812/SK6812 pulse data, APA102/SK9822 frames,
     HD108 frames, or other versioned protocol data;
   * transports submit that data through RMT, serial SPI, or a parallel-wave
     engine;
   * DMA is an offload capability of a transport, not a separate transport.
2. Expose one target-neutral parallel-wave capability. Its target adapters are:

   * ESP32: I2S LCD mode with DMA;
   * ESP32-S3: LCD_CAM with GDMA, not the ordinary audio I2S mode;
   * ESP32-C6: PARLIO with GDMA.
3. The output-driver ABI must advertise protocol/pixel formats, lane count,
   shared-timing constraints, physical minimum frame period, queue depth,
   asynchronous completion, DMA support, synchronization/skew guarantees,
   maximum qualified frame size, staging/internal-memory cost, and every
   claimed GPIO, bus, peripheral and DMA resource.
4. `auto` backend selection must be deterministic, expose the selected backend
   and reason in diagnostics, and allow a profile to force a backend for
   qualification or troubleshooting. Measured target/length/lane crossover
   points belong in versioned capability data rather than scattered conditionals.
5. Preallocate all descriptors, queues and staging buffers before render start.
   Prefer internal DMA-capable memory for active transfers; use PSRAM only where
   the target-specific DMA/cache behavior has passed the same stress tests.
6. Do not claim a performance gain that exceeds the wire protocol's physical
   limit. For example, an 800 kHz 24-bit one-wire lane takes approximately
   `pixels * 30 us + reset time`; parallel lanes improve aggregate throughput,
   not the frame time of one lane.
7. Production selection prioritizes zero corrupt frames and zero missed valid
   deadlines under Gate C load before CPU percentage, memory use, peripheral
   economy or headline throughput.

### Provisional backend recommendations

| Situation | ESP32 | ESP32-S3 | ESP32-C6 |
| --- | --- | --- | --- |
| One short one-wire strip | RMT | RMT | RMT |
| One long one-wire strip under Wi-Fi/radio load | SPI waveform encoding with DMA | RMT with DMA | PARLIO width 1 with GDMA; SPI with DMA fallback |
| Two to four identical one-wire lanes | Compare RMT with I2S parallel | Compare RMT DMA with LCD_CAM | PARLIO |
| Many identical one-wire lanes | I2S LCD-mode parallel DMA | LCD_CAM parallel GDMA | PARLIO parallel GDMA |
| One clocked APA102/SK9822/HD108-family strip | SPI with DMA | SPI with DMA | SPI with DMA |
| Several clocked strips sharing timing and clock | Compare multiline SPI with the parallel-wave engine | Compare multiline SPI with LCD_CAM | Compare multiline SPI with PARLIO |
| Lanes requiring independent timing/protocols | Independent RMT/SPI resources within measured limits | Independent RMT/SPI resources within measured limits | Independent RMT/SPI resources within the smaller C6 resource set |
| More lanes or timing groups than the SoC can guarantee | External RP2040/FPGA/CPLD output engine | External RP2040/FPGA/CPLD output engine | External RP2040/FPGA/CPLD output engine |

RMT remains the economical and flexible baseline for short strips and differing
lane timings. It is not presumed best for long interrupt-refilled transfers on
ESP32 or C6. SPI is the native choice for clocked protocols and a candidate for
robust DMA-backed one-wire waveform generation, but a no-chip-select LED strip
normally reserves the entire bus. The parallel-wave engine is the aggregate-
throughput choice when lanes can share protocol timing and frame cadence; it
also consumes the whole peripheral and potentially many GPIOs.

### Mandatory transport qualification before PR 3.4

Build a disposable HIL benchmark harness and record an ADR before production LED
transport code. Test RMT, RMT DMA where supported, SPI waveform encoding with
and without DMA, native clocked SPI, multiline SPI, and the applicable parallel
adapter on each target. Use an external capture/logic fixture where target
loopback cannot prove every lane.

The sweep must include:

* 1, 2, 4, 8 and 16 lanes where the target and board expose them;
* 1, 32, 256, 512 and 1,024 pixels per lane, plus the largest supported profile;
* WS2812/SK6812-class one-wire timing and representative APA102/SK9822/HD108
  clocked frames;
* idle, saturated bidirectional Wi-Fi, settings/metrics queries, flash writes,
  OTA/cache-disable windows and applicable radio coexistence;
* internal-memory staging and any proposed PSRAM/DMA path;
* mixed peripheral pressure from the resource broker's LED-heavy and full
  profiles.

Record CPU and encoder cycles per pixel, ISR count and worst refill latency,
internal/DMA/PSRAM usage, staging expansion, submission latency, sustainable
frame rate, missed deadlines, corrupt waveforms, lane start skew, queue behavior,
and claimed peripheral/GPIO resources. The ADR must select winners and measured
crossover points per target/situation, document rejected approaches, and define
the short qualification run that every backend must pass before the full Gate C
soak. Deviating from the provisional table requires measurements in that ADR.

## Implementation roadmap

Each numbered item should normally become one PR. Large component-parity items can become an epic containing one PR per component.

### Milestone 0 — Preserve observable V1 behavior

| PR  | Work                                                                                                              | Acceptance                                                           |
| --- | ----------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------- |
| 0.1 | Document current OSC paths, settings, playback files, communication formats and component list.                   | Public compatibility matrix checked into`docs/v2/`.                |
| 0.2 | Capture representative V1 fixtures: settings, OSCQuery tree, serial messages, ESP-NOW packets and playback files. | Fixtures can be consumed by host-side tests without hardware.        |
| 0.3 | Write ADRs for component lifecycle, resource ownership, threading, memory policy, persistence and compatibility.  | No production V2 implementation begins before these contracts exist. |

This does not require faithfully reproducing V1 bugs or undocumented internal coupling.

### Milestone 1 — Buildable foundation

| PR  | Work                                                                                                        | Acceptance                                                                                          |
| --- | ----------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------- |
| 1.1 | Create the ESP-IDF project, dependency lock, formatting rules and CI matrix.                                | Minimal firmware builds for ESP32, S3 and C6.                                                       |
| 1.2 | Implement component descriptors, registry and lifecycle: construct, validate, start, suspend, resume, stop. | Host tests cover ordering, duplicate IDs, dependency cycles and failure cleanup.                    |
| 1.3 | Implement typed parameters, actions, events and metadata.                                                   | A synthetic component produces a complete machine-readable descriptor.                              |
| 1.4 | Add a resource broker for GPIO, RMT, SPI, I2C, UART, timers, DMA, PSRAM and radio resources.                | Conflicting board/profile configurations fail at build time where possible and boot time otherwise. |
| 1.5 | Add the scheduler and bounded event bus.                                                                    | Queue overflow is visible and deterministic; real-time work has no unbounded execution.             |

**Gate A:** the same core and component model boots on all three chip families.

### Milestone 2 — Settings, diagnostics and basic control

| PR  | Work                                                                                                       | Acceptance                                                                             |
| --- | ---------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------- |
| 2.1 | Add versioned per-component settings backed by NVS.                                                        | Interrupted saves retain either the previous or new valid state.                       |
| 2.2 | Add LittleFS and optional SD storage services with atomic replacement.                                     | Files survive forced resets during update tests.                                       |
| 2.3 | Add settings migration and V1 settings import.                                                             | Golden V1 fixtures import into the expected V2 component values.                       |
| 2.4 | Implement structured logging, runtime log levels, metrics, reset-cause reporting, coredumps and safe mode. | Boot loops enter a diagnosable recovery mode.                                          |
| 2.5 | Implement the common transport envelope and Serial/USB transport.                                          | Parameters and actions can be controlled through a host test utility.                  |
| 2.6 | Implement Wi-Fi station/AP management and serial/SoftAP provisioning.                                      | A blank device can be configured without reflashing.                                   |
| 2.7 | Implement OSC UDP and OSCQuery HTTP/WebSocket discovery, including mDNS advertisements for `_osc._udp` and `_oscjson._tcp`. | The hierarchy is generated exclusively from the registry, passes protocol fixtures, and is discoverable through DNS-SD/mDNS. |

### Milestone 3 — First complete vertical slice

| PR  | Work                                                                         | Acceptance                                                                       |
| --- | ---------------------------------------------------------------------------- | -------------------------------------------------------------------------------- |
| 3.1 | Build the schema-driven web application shell.                               | Adding a test component requires no hand-written control panel.                  |
| 3.2 | Add filesystem management and separately updatable web assets.               | UI assets can be replaced without reflashing the application partition.          |
| 3.3 | Implement A/B OTA, rollback, image/profile validation and boot confirmation. | Failed and interrupted updates recover automatically.                            |
| 3.4 | After the transport qualification ADR, add the first functional LED backend—single-strip WS2812/SK6812 through native RMT. RMT is a portable vertical-slice baseline, not the production-backend decision. | Web and OSC control a real strip on each reference target; captured output is byte/timing-correct and the qualification ADR is linked. |
| 3.5 | Add browser installation using esptool-js/Launchpad-compatible manifests.    | A factory device can be flashed, provisioned, opened and updated from a browser. |
| 3.6 | Add broker-backed pin selectors and a complete pin/reservation inspector. Every pin-valued control lists all board-declared pins with its current owner and compatibility; component-owned conflicts offer explicit atomic swap or unassign-and-move operations, while system/critical reservations remain visible but unavailable. Model I2C pins as a shared bus assignment rather than conflicting per-device claims. | The UI cannot silently double-book an exclusive pin; stale or invalid changes roll back, all reservations remain inspectable, and compatible I2C members share one clearly identified bus without false conflicts. |

**Gate B:** browser install → Wi-Fi setup → OSCQuery discovery → conflict-safe pin assignment/reservation inspection → LED control → settings save → OTA rollback works end to end.

### Milestone 4 — Production LED engine

| PR  | Work                                                                                 | Acceptance                                                                                          |
| --- | ------------------------------------------------------------------------------------ | --------------------------------------------------------------------------------------------------- |
| 4.1 | Implement the pixel surface, protocol-encoder boundary and output-driver ABI.        | Drivers advertise formats, lane/timing constraints, physical frame period, buffering, DMA, synchronization, qualified limits, memory and claimed resources. |
| 4.2 | Implement the compositor and`stream`, `playback`, `script`, `system` layers. | Layer priority, opacity and blend behavior have deterministic host tests.                           |
| 4.3 | Add linear-light color transforms, channel ordering, calibration and correction.     | Known color vectors produce golden outputs.                                                         |
| 4.4 | Add asynchronous frame submission, preallocated DMA/staging pools and deadline metrics. | The render task performs zero allocations after startup; overload and completion behavior are bounded and observable. |
| 4.5 | Add native SPI-DMA clocked-strip support: APA102/SK9822, HD108 and compatible protocols. | Protocol analyzer or loopback fixtures verify frame layout, clock limits and timing.                 |
| 4.6 | Implement the qualified production transport set and deterministic selector: RMT/RMT-DMA, SPI-encoded one-wire, target-neutral parallel-wave adapters, and an optional FastLED compatibility backend. This may be an epic with one reviewable PR per transport. | Each selected backend passes its ADR qualification range; `auto` reports its reason, forced selection is testable, resource conflicts fail cleanly, and every backend uses the same output ABI. |
| 4.7 | Implement streaming and playback, including legacy import.                           | Network streaming does not block LED output; corrupted playback files fail safely.                  |
| 4.8 | Add Art-Net/DMX compatibility with Art-Net node advertisement/discovery, followed by optional E1.31/DDP components. | An Art-Net controller can discover the node; each compatibility component is independently removable from minimal builds. |

**Gate C:** sustained Wi-Fi traffic cannot corrupt LED timing, exhaust queues or cause monotonic heap loss.

### Milestone 5 — Battery, power and radios

| PR  | Work                                                                                                           | Acceptance                                                                        |
| --- | -------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------- |
| 5.1 | Implement battery sensing, filtered state-of-charge inputs and board-specific calibration.                     | Calibration and low-battery states are exposed through the registry.              |
| 5.2 | Implement LED current estimation, configurable power budgets, smoothing and hard safety limits.                | Output never exceeds its configured calculated budget.                            |
| 5.3 | Add ESP-IDF power-management locks, light/deep sleep coordination and wake sources.                            | Each board profile records measured idle and sleep behavior.                      |
| 5.4 | Implement`RadioManager` profiles and explicit memory/resource policies.                                      | UI clearly distinguishes live suspension from reboot-required memory reclamation. |
| 5.5 | Add NimBLE serial/service transport. Add Classic Bluetooth only for original ESP32 profiles.                   | BLE can be excluded entirely and suspended when unused.                           |
| 5.6 | Implement ESP-NOW V1/V2 negotiation, fragmentation, sequence numbers, acknowledgements and duplicate handling. | Tests cover V1↔V1, V2↔V2 and mixed-fleet behavior.                              |
| 5.7 | Implement fleet clock synchronization, leader election and scheduled cues.                                     | Nodes recover from leader loss without blocking normal computer control.          |

### Milestone 6 — WASM

| PR  | Work                                                                                      | Acceptance                                                                             |
| --- | ----------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------- |
| 6.1 | Create a target-representative wasm3/WAMR benchmark harness.                              | ADR records code size, RAM, execution speed, fault handling and maintenance status.    |
| 6.2 | Implement a runtime-neutral WASM service and select WAMR unless benchmarks disqualify it. | Runtime can be replaced without modifying components.                                  |
| 6.3 | Add bounded memory, stack, execution budget, cancellation and fault isolation.            | Infinite loops, traps and invalid pointers cannot stall LED or communication tasks.    |
| 6.4 | Implement UTF-8 strings as checked pointer-and-length values.                             | Tests cover empty, long, invalid and unterminated inputs.                              |
| 6.5 | Implement component-owned capability providers.                                           | Components register their own functions instead of editing a central function library. |
| 6.6 | Allow scripts to declare parameters, actions and events.                                  | Script-defined controls automatically appear in OSCQuery and the web UI.               |
| 6.7 | Add generated script SDK bindings and the LED script layer.                               | Example scripts compile and run against versioned capability manifests.                |

**Gate D:** a faulty script is terminated and diagnosed while LED output, settings and networking remain operational.

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

* Board/resource declaration
* Component schema
* Settings migration if relevant
* OSCQuery and web exposure
* Host tests
* At least one hardware test record
* Flash/RAM impact for every enabled profile

### Milestone 8 — Firmware Kitchen

| PR  | Work                                                                                | Acceptance                                                                        |
| --- | ----------------------------------------------------------------------------------- | --------------------------------------------------------------------------------- |
| 8.1 | Define board, feature and profile manifest schemas.                                 | Invalid combinations report understandable conflicts.                             |
| 8.2 | Generate`sdkconfig`, partition tables, component selection, pins and web schemas. | Generated files are reproducible from a committed manifest.                       |
| 8.3 | Add a local CLI and reproducible containerized builder.                             | Identical inputs produce identical application artifacts.                         |
| 8.4 | Build the web Kitchen for board selection, capabilities and configuration.          | It exports a manifest and installable firmware bundle.                            |
| 8.5 | Add CI build caching and a release artifact matrix.                                 | Minimal, standard, LED-heavy, sensor-heavy and full profiles build automatically. |

### Milestone 9 — Hardening and cutover

* Fuzz settings, OSC, OSCQuery, file, ESP-NOW and WASM boundaries.
* Run reset, brownout, network-loss and corrupted-storage fault injection.
* Run multi-day LED/network/ESP-NOW soak tests.
* Record stack high-water marks and heap trends.
* Validate OTA interruption at each update stage.
* Test all canonical profiles on all supported targets.
* Publish board bring-up and component-authoring guides.
* Ship `v2.0.0-alpha`, then beta, before changing the repository default.
* Move or archive V1 only after explicit approval.

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

## Recommended first Codex assignment

Start with Milestone 0 only:

> Create the V2 planning foundation on a new `v2` branch. Preserve the existing firmware unchanged. Add `docs/v2`, architecture decisions, the complete V1 component/behavior inventory, compatibility fixtures, target profiles, measurable performance/stability gates, and a PR/issue dependency map. Do not implement production V2 firmware in this PR.

That gives later Codex sessions stable contracts instead of allowing architecture to drift as features are added.
