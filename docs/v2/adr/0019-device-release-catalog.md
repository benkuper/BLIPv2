# ADR-0019: Device-specific HTTPS release catalog

- Date: 2026-10-08
- Status: Accepted for the release contract; device worker/installation qualification open
- Related: work packages 3.8, 3.9 and 8.5

The default endpoint is `https://www.goldengeek.org/blip/update`. Devices add
percent-encoded GET fields: `schema=1`, `project`, `board`, `target`, `layout`,
`profile`, `channel`, `flash_bytes`, `features`, `api`, `fw_code`, `fw_version`
and `web_code`. No MAC, SSID, password, sensor value or personal identifier is
included. Identity text is bounded printable ASCII. Feature masks distinguish
builds with different enabled components even when their profile labels match.

The JSON response contains exactly `schema=1`, the nine compatibility fields
and independent `firmware`/`web` artifacts. Compatibility must match all nine
fields exactly. An unpublished identity returns both artifacts as `null`;
clients must distinguish this from a published, already-installed release.
Each non-null artifact contains exactly `code`, `version`, `bytes`, `url`,
`sha256`, `minimum_other_code`. Codes are monotonic unsigned 32-bit release
sequences; labels may include alpha/beta suffixes. Web bundle codes retain the
existing `major * 1000000 + minor * 1000 + patch` representation.

`minimum_other_code` is the minimum installed version code of the other artifact.
A newer artifact is eligible only when that prerequisite is already installed.
Circular prerequisites cannot enable either update. The device checks actual
OTA partition capacity and the 256 KiB web bundle bound before downloading.
Firmware retains native image/chip/signature validation and A/B confirmation;
web assets retain bundle validation and atomic replacement. A SHA-256 digest
from the authenticated catalog must match the complete downloaded artifact
before activation. The catalog is limited to 4096 bytes, stores strings in owned
fixed buffers and rejects duplicates, unknown keys, invalid integer forms and
oversized text. Failed decoding clears the result.

URLs require HTTPS, a DNS/IPv4 host, a valid optional port and ASCII syntax.
Credentials, fragments, whitespace, backslashes and invalid percent escapes are
rejected. IPv6 literals are not supported by this version. Device HTTPS must
verify certificates and their time validity; redirects must not silently weaken
this policy. SHA-256 binds artifacts to the authenticated catalog; it does not
replace an independently signed catalog or secure-boot image policy.

The portable Python handler uses the same schema, rejects duplicate query/JSON
fields, reloads the operator's catalog on each request and does not log GET
values. It exposes only catalog-listed artifact paths beneath `/blip/releases/`
and checks resolved containment. Connections and TLS handshakes have ten-second
timeouts. Startup requires a TLS certificate/key. Hosting this handler on
GoldenGeek remains a deployment step; no live catalog or release publication
is claimed by this contract checkpoint.

The native HTTPS worker, update center, automatic policy and
interruption/corruption/rollback hardware
qualification remain required before completing 3.8/3.9 or Gate B.

The subsequent image checkpoint embeds a 204-byte `BLIPREL1` descriptor directly
after the native app descriptor, at image byte 288. It stores schema, monotonic
release code, configured flash bytes, feature mask, control API, project, board,
target, layout and profile. Fixed ASCII text has zero padding. Download prefix
validation compares these fields plus native app project/version against the
selected release before activation. The catalog now requires firmware to be at
least 492 bytes. Native chip/signature/image validation remains mandatory.

Feature bits 0..7 are WASM, BLE, Classic BT, ESP-NOW, fleet, Art-Net, DDP and
E1.31. Board identity distinguishes onboard hardware; future feature mappings
must extend this documented mask. Firmware builds validate nonzero u32 release
codes, bounded profile identifiers and the declared 4/8 MB partition filenames.
Configured flash/layout identity describes the generated installation layout;
it does not claim the physical flash was independently measured on each board.

The local packaging CLI reads actual image metadata, checks target/header
consistency and partition capacity, verifies the web bundle, computes file
digests and copies immutable digest-named artifacts before atomically replacing
the index. It rejects changes under an existing release code. Firmware-only
publication retains the independent web entry. Publication uses one writer.
The user selected a separate API server; the Python handler is the deployment
target. Remote deployment and native HTTPS installation remain open.
