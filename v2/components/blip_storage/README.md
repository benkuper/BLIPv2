# blip_storage

This component owns persistence under ADR-0005 and ADR-0006. Work package 2.1
provides a versioned, per-component settings service with an injectable blob
backend and an ESP-IDF NVS implementation. Work package 2.2 adds bounded atomic
file records over internal LittleFS and optional mounted SD storage. Work
package 2.3 adds ordered schema migrations. V1 import is retained only in
historical host tools and is not started by the firmware.
Work package 3.2 adds a deterministic, verified web-asset bundle managed in
LittleFS independently of the application image.

`SettingsStore` writes an inactive generation, reads it back, and only then
updates an integrity-checked commit record. Reads prefer the committed
generation and recover from corrupt/torn records using the other valid slot.
The caller supplies the fixed scratch buffer; the service performs no heap
allocation. The production component reserves 4 KiB and exposes the typed
service ID `storage.settings` through the registry.

The default backend uses namespace `blip_v2` and reads V2 component records.

The service is single-owner and must run in the storage/maintenance scheduling
class. Components pass payloads through this service and never access NVS
directly.

The internal service uses verified temporary-file replacement. The optional SD
service uses two full generations and a checked pointer, tolerating filesystems
whose replacement durability is weaker. Neither service automatically formats
nonblank unknown media. The standard 4 MiB layout has 640 KiB of internal
LittleFS (512 KiB in the BLE layout); Ball's 8 MiB layout has 1.75 MiB.
Generic board manifests assign no external-storage pins.

`AutomaticFileStore` streams bulk files through two independently checked
generations on the selected filesystem. Mounted external storage is preferred;
missing external files can fall back to internal storage. An admitted handle
keeps its medium until close/cancel. Filesystem I/O errors are not treated as
missing files. Directory pages merge logical names with bounded caller-supplied
space; payload integrity is checked when opening files.

`WebAssetStore` verifies a maximum 256 KiB candidate's format, layout and layered
CRC-32 values before publishing through that file service. An interrupted or
invalid update retains the previous bundle. Factory flashing writes the complete
UI into a separate internal filesystem image, and a checked copy migrates to
external media when present. The full UI is absent from the application image.
Missing/corrupt UI opens the small firmware-resident first-run downloader. The
component publishes `storage.web_assets` plus bundle version, count and size.

See [the 2.1 work-package record](../../../docs/v2/work-packages/2.1-versioned-nvs-settings.md),
[the 2.2 record](../../../docs/v2/work-packages/2.2-atomic-file-storage.md),
[the 2.3 record](../../../docs/v2/work-packages/2.3-settings-migration-v1-import.md),
and [the 3.2 record](../../../docs/v2/work-packages/3.2-filesystem-web-assets.md)
for persisted layouts, recovery rules, tests, and measured costs.
