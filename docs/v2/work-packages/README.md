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
| [2.3](2.3-settings-migration-v1-import.md) | Ordered schema migrations and a bounded, journaled, physically qualified V1 settings import |
| [2.4](2.4-diagnostics-safe-mode.md) | Bounded structured diagnostics, retained coredumps, and physically qualified boot-loop recovery |
| [2.5](2.5-serial-transport.md) | Versioned common envelope, bounded Serial/USB control, host CLI, and normal/recovery hardware qualification |
| [2.6](2.6-wifi-provisioning.md) | Versioned Wi-Fi station/AP management with serial and SoftAP provisioning |
| [2.7](2.7-osc-oscquery.md) | Bounded OSC UDP and registry-generated OSCQuery HTTP/WebSocket discovery |

## Milestone 3

| Work package | Result |
| --- | --- |
| [3.1](3.1-schema-web-shell.md) | Dependency-free controls generated from the live registry schema |
| [3.2](3.2-filesystem-web-assets.md) | Deterministic, verified web bundles with atomic LittleFS replacement and on-device serving |
