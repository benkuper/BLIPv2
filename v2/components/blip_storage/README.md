# blip_storage

This component owns persistence under ADR-0005 and ADR-0006. Work package 2.1
provides a versioned, per-component settings service with an injectable blob
backend and an ESP-IDF NVS implementation.

`SettingsStore` writes an inactive generation, reads it back, and only then
updates an integrity-checked commit record. Reads prefer the committed
generation and recover from corrupt/torn records using the other valid slot.
The caller supplies the fixed scratch buffer; the service performs no heap
allocation. The production component reserves 4 KiB and exposes the typed
service ID `storage.settings` through the registry.

The default backend uses namespace `blip_v2`. It never erases or writes V1's
`blip/settings` value. V1 import belongs to work package 2.3.

The service is single-owner and must run in the storage/maintenance scheduling
class. Components pass payloads through this service and never access NVS
directly.

See [the 2.1 work-package record](../../../docs/v2/work-packages/2.1-versioned-nvs-settings.md)
for the persisted byte layout, recovery rules, tests, and measured costs.
