# BLIP schema-driven web shell

This dependency-free browser application builds its component controls from
the device's live OSCQuery tree. Registry additions appear without web source
changes. Its update center separately shows installed/published firmware and
interface versions, download progress, cancellation and independent installs.

Simple mode opens by default with component-declared main controls, switches,
sliders and real recent-reading graphs/meters. Advanced exposes all published
controls plus resource assignments, with topics and search. Component hints and
the remaining scope are in [ADR-0018](../../docs/v2/adr/0018-device-web-presentation.md).

The app fetches `/?HOST_INFO` and `/?config=1`, then controls
the device through binary OSC messages on the root WebSocket. Parameters render
as booleans, numbers, strings, password fields, enumerated selects, or read-only
values. Actions render argument fields from `BLIP_FIELDS`; events render as live
values. All labels are inserted with `textContent`, write-only values are never
displayed, and protocol/schema inputs are bounded.

Visible pages poll every three seconds with backoff, refresh dynamic schema
generations and preserve focused input drafts during value updates. Module
startup is sequential to fit the device's four HTTP sessions. The home page
works on station/AP; `/setup` keeps independent Wi-Fi recovery available.

## Local use

Serve the directory from an HTTP origin:

```powershell
python -m http.server 8080 --directory v2/web
```

Open `http://localhost:8080/?device=http://192.0.2.8`, replacing the device URL
as needed. Without `device`, the shell uses its own origin, which is also its
on-device behavior.

Run the deterministic model, transport, and parser suite with:

```powershell
npm test --prefix v2/web
```

Verify that the checked-in factory bundle exactly matches these sources with:

```powershell
npm run build --prefix v2/web
```

The maintenance panel accepts a raw target-matched ESP-IDF application image,
derives its project/version metadata and SHA-256 in the browser, and uploads it
to the A/B OTA endpoint. Hashing also works on device HTTP origins without
SubtleCrypto; it yields while processing larger images. The device checks the
embedded board/layout/profile identity before writing an uploaded app.
Factory flashing uses the separate complete images and
manifests under [`../installer/`](../installer/README.md).

The packer can also produce and atomically upload a separately versioned bundle
to a device on a trusted local network:

```powershell
python v2/tools/web/pack_web_assets.py --source v2/web `
  --output build/web/assets.bundle --upload http://192.0.2.8
```

## Scope boundary

Work package 3.1 defines the source shell and its registry/OSCQuery contract.
Work package 3.2 introduced deterministic compression, filesystem serving,
and verified atomic replacement. Current
builds write that bundle directly into the factory filesystem and omit the full
UI from the application binary. Only the small first-run downloader is compiled
into firmware; an empty factory filesystem can download the UI online. Bundle
updates are not cryptographically signed in this package. The current
HTTP/WebSocket control and asset-update surfaces are unauthenticated and
intended only for a trusted local network.

See [the 3.2 work-package record](../../docs/v2/work-packages/3.2-filesystem-web-assets.md)
for the binary format, HTTP routes, recovery rules, and build evidence.
