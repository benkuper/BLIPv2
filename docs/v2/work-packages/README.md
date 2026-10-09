# V2 implementation work packages

## Milestone 1

| Work package | Result |
| --- | --- |
| [1.1](1.1-bootstrap.md) | Reproducible ESP-IDF 6.0.2/C++20 project and CI matrix |
| [1.2](1.2-registry-lifecycle.md) | Deterministic component registry and lifecycle |
| [1.3](1.3-descriptors.md) | Typed, versioned machine-readable descriptors |
| [1.4](1.4-resource-broker.md) | Bounded resource leases and manifest conflict checks |
| [1.5](1.5-scheduler-event-bus.md) | Fixed-capacity scheduler and typed event bus |
| [Milestone evidence](milestone-1-evidence.md) | Host tests, clean-build hashes, sizes, and open hardware gates |

These packages add only V2 paths. They do not change V1 source, persisted state,
wire protocols, or public OSC paths.

## Milestone 2

| Work package | Result |
| --- | --- |
| [2.1](2.1-versioned-nvs-settings.md) | Versioned per-component NVS records with two-slot interrupted-save recovery |
| [2.2](2.2-atomic-file-storage.md) | Versioned atomic LittleFS files and an optional two-slot SD service with bounded recovery |
| [2.2 media extension](2.2-automatic-media-storage.md) | Automatic external/internal bulk files for web assets, scripts, playback and sequences; integration/qualification in progress |
| [2.3](2.3-settings-migration-v1-import.md) | Ordered V2 schema migrations; prior V1 import evidence retained as historical record |
| [2.4](2.4-diagnostics-safe-mode.md) | Bounded structured diagnostics, retained coredumps, and physically qualified boot-loop recovery |
| [2.5](2.5-serial-transport.md) | Versioned common envelope, bounded Serial/USB control, host CLI, and normal/recovery hardware qualification |
| [2.6](2.6-wifi-provisioning.md) | Versioned Wi-Fi station/AP management with serial and SoftAP provisioning |
| [2.7](2.7-osc-oscquery.md) | Bounded OSC UDP and registry-generated OSCQuery HTTP/WebSocket discovery |

## Milestone 3

| Work package | Result |
| --- | --- |
| [3.1](3.1-schema-web-shell.md) | Dependency-free controls generated from the live registry schema |
| [3.2](3.2-filesystem-web-assets.md) | Deterministic, verified web bundles with atomic LittleFS replacement and on-device serving |
| [3.3](3.3-ab-ota-rollback.md) | Profile-checked A/B firmware updates with late boot confirmation and automatic rollback |
| [3.4](3.4-native-rmt-strip.md) | Native single-strip RMT vertical slice and qualification boundary |
| [3.5](3.5-browser-installer.md) | Browser-compatible factory artifacts and on-device firmware update flow |
| [3.6](3.6-pin-reservation-ui.md) | Complete pin/reservation inspection with conflict-safe reassignment and shared-bus handling |
| [3.7–3.9](3.7-device-web-and-self-update.md) | Required Simple/Advanced graphical web app and device-specific firmware/web self-updates; implementation checkpoints below, full qualification open |
| [3.7 browser foundation](3.7-simple-advanced-foundation.md) | Simple/Advanced modes, real graphs and automatic script-schema refresh; seven builds and three-family device-served browser trials passed; remaining diagnostic/scalar surfaces and self-updates open |
| [3.7 device identity](3.7-device-identity.md) | Editable saved names, board type, live name-based mDNS and web identity; six-board identity trials passed, Tab access and full milestone qualification open |
| [3.8/3.9 native updates and first run](3.8-native-update-and-first-run.md) | Independent website updates, filesystem-only full UI and automatic first run; HUZZAH32 browser/native OTA and mismatch checks passed, seven-board/recovery/coexistence qualification open |
| [3.8 update recovery](3.8-update-recovery.md) | Socket capacity, automatic browser reconnection, bounded file buffering and fault-injected update trials; seven-board/coexistence qualification open |
| [3.8 reserved worker and RAM](3.8-update-recovery.md#reserved-worker-and-sd-progress-checkpoint) | Preallocated OTA task, compact copied controls and responsive SD status; six-board native/fleet and Ball SD recovery trials passed; extra-client, Tab and full milestone gates remain open |
| [3.8 transport stacks](3.8-transport-stack-qualification.md) | UDP persisted-name/NVS overflow fixed; six boards passed 1,035 network/script/provisioning checks each with unchanged static RAM; Tab and update coexistence remain open |

## Milestone 4

| Work package | Result |
| --- | --- |
| [4.1](4.1-led-output-abi.md) | Linear pixel surfaces, encoder boundary, and capability-rich output ABI |
| [4.2](4.2-led-compositor.md) | Deterministic stream/playback/script/system compositor |
| [4.3](4.3-led-color.md) | Linear-light matrix, calibration, transfer, and channel-order pipeline |
| [4.4](4.4-led-async.md) | Preallocated async frame pool with bounded overload and deadline metrics |
| [4.5](4.5-led-clocked-spi.md) | APA102/SK9822/HD108 encoders and asynchronous native SPI-DMA output |
| [4.6](4.6-led-backends.md) | Fail-closed deterministic backend selection and common transport adapters |
| [4.7](4.7-led-stream-playback.md) | Bounded stream layer, versioned V2 playback, and clock |
| [4.8](4.8-led-network-compat.md) | Removable Art-Net, DDP, and optional E1.31 runtimes |
| [Milestone evidence](milestone-4-evidence.md) | Host goldens, target builds, sustained soak, and open Gate C HIL boundary |

## Milestone 5

| Work package | Result |
| --- | --- |
| [5.1](5.1-battery.md) | HUZZAH32 calibrated ADC and filtered voltage estimate; other board calibration open |
| [5.2](5.2-led-current-limiting.md) | Encoded-frame calculated current limit and six-board builds; physical current calibration open |
| [5.3](5.3-pm-dfs.md) | DFS with pixel-frame CPU locks; physical power measurements open |
| [5.4](5.4-wifi-radio-profiles.md) | Live Wi-Fi suspension and reboot-applied driver memory reclamation; BLE and ESP-NOW profiles open |
| [5.5 Bluetooth transports](5.5-ble-transport-foundation.md) | NimBLE GATT on six boards and Classic SPP on two original ESP32 boards; remaining coexistence and power qualification open |
| [5.6 ESP-NOW V2](5.6-espnow-v2.md) | Bounded V2 peer control, ACK/retry and duplicate suppression; seven-board build and Ball-to-peer HIL passed |
| [5.7 Routerless fleet](5.7-routerless-fleet.md) | ESP-NOW broadcast election/clock/cues; 4,096-listener host case, seven-board build/flash and routerless recovery gates passed; physical timing and radio scale qualification open |

## Milestone 6

| Work package | Result |
| --- | --- |
| [6.1](6.1-wasm-runtime-benchmark.md) | Nine standalone target benchmarks; metered WAMR selected in ADR-0008; service and production follow in 6.2/6.3 |
| [6.2](6.2-wasm-service.md) | Runtime-neutral service and actual WAMR adapter; 390 checks on three chip families and repeated lifecycle passed; production slice in 6.3 |
| [6.3](6.3-wasm-production-worker.md) | Opt-in supervised worker; Ball trap/cancellation/queue and LED/network/settings coexistence passed; full Gate D open |
| [6.3 profile memory](6.3-wasm-profile-memory.md) | Seven production builds and declared memory/policy trials; selected ESP32 traffic passed; M5StickC large serial burst gap remains |
| [6.3 native lifecycle](6.3-wasm-native-lifecycle.md) | 3,716 checks on three families; real task stop/restart, active/queued cancellation and injected startup cleanup passed; production stress and cold/global OOM remain open |
| [6.3 M5StickC UART localization](6.3-m5stickc-uart-localization.md) | Three missing-byte replies correlate with valid full UART driver admissions; loss beyond encoding is reproduced; driver/physical/USB/host cause and serial burst gate remain open |
| [6.4 checked UTF-8](6.4-wasm-checked-utf8.md) | Checked unsigned pointer/length copies, UTF-8 validation and generation guards; 23 host tests, 3,069 native checks and seven production builds passed; provider bindings follow in 6.5 |
| [6.5 provider contract](6.5-component-provider-contract.md) | Component-owned versioned descriptors and portable catalog; 24 host suites, 619 XIAO native checks and seven production builds passed; guest-to-native bridge and production providers remain pending |
| [6.5 native bridge](6.5-native-import-bridge.md) | Checked guest-to-native calls qualified on three families; production providers follow |
| [6.5 production providers](6.5-production-providers.md) | Versioned LED/fleet providers and copied scheduled actions; seven builds and three-family production HIL passed |
| [6.6 declarations](6.6-script-declaration-contract.md) | Owned declaration decoding/projection; live integration follows |
| [6.6 registry interfaces](6.6-dynamic-registry-interfaces.md) | Leased dynamic controls and owned transport replies; surrogate and static production controls qualified |
| [6.6 owned store](6.6-script-control-store.md) | Typed values, copied queues, leased retirement and actual callback/global checks; production owner/UI integration pending |
| [6.6 production controls](6.6-production-live-controls.md) | Live registry publication and copied supervised actions; seven builds, three-family production trials and provider regressions passed; guest events/transport/UI/SDK pending |
