# blip_storage

This component owns persistence under ADR-0005 and ADR-0006. Work package 2.1
provides a versioned, per-component settings service with an injectable blob
backend and an ESP-IDF NVS implementation. Work package 2.2 adds bounded atomic
file records over internal LittleFS and optional mounted SD storage. Work
package 2.3 adds ordered schema migrations and a bounded V1 settings importer.

`SettingsStore` writes an inactive generation, reads it back, and only then
updates an integrity-checked commit record. Reads prefer the committed
generation and recover from corrupt/torn records using the other valid slot.
The caller supplies the fixed scratch buffer; the service performs no heap
allocation. The production component reserves 4 KiB and exposes the typed
service ID `storage.settings` through the registry.

The default backend uses namespace `blip_v2`. The V1 adapter opens
`blip/settings` read-only, validates the entire MessagePack blob, and retains it
after a journaled import. Existing V2 component records always take precedence.

The service is single-owner and must run in the storage/maintenance scheduling
class. Components pass payloads through this service and never access NVS
directly.

The internal service uses verified temporary-file replacement. The optional SD
service uses two full generations and a checked pointer, tolerating filesystems
whose replacement durability is weaker. Neither service automatically formats
nonblank unknown media. The generic targets enable only the internal 384 KiB
LittleFS partition and assign no SD pins.

See [the 2.1 work-package record](../../../docs/v2/work-packages/2.1-versioned-nvs-settings.md),
[the 2.2 record](../../../docs/v2/work-packages/2.2-atomic-file-storage.md), and
[the 2.3 record](../../../docs/v2/work-packages/2.3-settings-migration-v1-import.md)
for persisted layouts, recovery rules, tests, and measured costs.
