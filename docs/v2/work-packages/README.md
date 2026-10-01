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
