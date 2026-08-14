# BLIP schema-driven web shell

This dependency-free browser application builds its entire control surface from
the device's live OSCQuery tree. It contains no component IDs, OSC paths, or
component-specific panels. Registry additions therefore appear without web
source changes.

The shell fetches `/?HOST_INFO` and `/?config=1` (or `config=0`), then controls
the device through binary OSC messages on the root WebSocket. Parameters render
as booleans, numbers, strings, password fields, enumerated selects, or read-only
values. Actions render argument fields from `BLIP_FIELDS`; events render as live
values. All labels are inserted with `textContent`, write-only values are never
displayed, and protocol/schema inputs are bounded.

## Local use

Serve the directory from an HTTP origin:

```powershell
python -m http.server 8080 --directory v2/web
```

Open `http://localhost:8080/?device=http://192.0.2.8`, replacing the device URL
as needed. Without `device`, the shell uses its own origin, which is the final
on-device behavior planned for work package 3.2.

Run the deterministic model, transport, and parser suite with:

```powershell
npm test --prefix v2/web
```

## Scope boundary

Work package 3.1 commits the source shell and its registry/OSCQuery contract.
It does not embed, compress, sign, upload, or serve these assets from firmware;
versioned asset bundles and filesystem management belong to work package 3.2.
The current HTTP/WebSocket control surface is unauthenticated and intended only
for a trusted local network.
