# GoldenGeek release endpoint

`blip_release_web_hil.py` qualifies a device's native web download from a locally
published catalog. Supply `--port`, `--mac`, `--endpoint`, `--build`, `--flash-log`,
`--bundle`, and `--report`. It checks compatibility, HTTP success, installed bundle
metadata, unchanged boot sequence and worker stack margin, then restores the saved
endpoint and automatic-update policy. It uses the existing shared network and does
not change PC Wi-Fi. Publish a strictly newer bundle before running it.

`release_server.py` is a dependency-free Python HTTP handler for
`/blip/update` and optional catalog-listed `/blip/releases/` artifacts. It is
ready to connect to a host; this repository does not deploy it to GoldenGeek.
The endpoint is public and requires no API password or API key. The user will
put it online after local simulation; live deployment is outside this work.
Contract: [ADR-0019](../../../docs/v2/adr/0019-device-release-catalog.md).

Run behind a reverse proxy that preserves the original GET query, or terminate
TLS directly with a valid certificate for the device-visible hostname:

```powershell
python v2/tools/ota/release_server.py --catalog releases.json --artifacts ./published --cert fullchain.pem --key privkey.pem
```

The listener defaults to `127.0.0.1:8088`; TLS is optional.
Certificate/key files are operator inputs and must remain outside tracked source.
The catalog is loaded on each GET; replace it atomically when publishing a release.
Publish immutable artifact files first, then publish the catalog that names them.

An index has `schema: 1` and a `releases` array. Each row has these fields:

```json
{
  "project": "blip-v2", "board": "creators-ball-v2", "target": "esp32c6",
  "layout": "ota-8mb-v1", "profile": "minimal", "channel": "stable",
  "flash_bytes": 8388608, "features": 123, "api": 1,
  "firmware": null, "web": null
}
```

The feature mask above is an illustrative value, not a published production
profile. Replace an artifact's `null` with `code`, `version`, `bytes`, `url`,
`sha256` (64 lowercase hex characters) and `minimum_other_code`.
Use the actual file size/digest and exact device identity. A row cannot be
duplicated. A request with no matching row returns both artifacts as `null`.
Do not infer that an unpublished device is current.

The optional artifact root serves only paths explicitly named in the catalog,
without query strings. Path traversal and resolved symlink escapes are rejected.
The current single-request server bounds slow connections but should sit behind
a production proxy with request limits when exposed publicly.

Validate locally:

```powershell
$env:BLIP_RELEASE_CATALOG_TEST_EXE = (Resolve-Path build/host/Debug/blip_release_catalog_tests.exe).Path
python v2/tools/ota/test_release_server.py -v
```

Setting that environment variable also runs an actual HTTP response through the
C++ device parser and compatibility validator. Unit tests use local HTTP only;
the server entry point supports plain HTTP and optional TLS.

Package an actual generated firmware image and the independently versioned web
bundle before deploying them to the separate API server:

```powershell
python v2/tools/ota/release_publish.py --firmware build/m6-production-ball/blip-v2.bin --web v2/components/blip_storage/factory_web.bundle --catalog published/releases.json --artifacts published/files
```

The tool reads the firmware's embedded board/layout/profile/feature/API identity
and its native app version. It verifies the web bundle, derives both version
codes, computes complete SHA-256 digests, copies immutable digest-named artifacts
and replaces the index atomically after verifying those copied files. Firmware
release codes come from `-DBLIP_RELEASE_SEQUENCE=N`; increase the code when image
or release metadata changes. `-DBLIP_RELEASE_PROFILE=NAME` sets the profile.
Publishing firmware alone preserves the existing independent web entry.

Keep publication serialized to one writer. The tool packages local artifacts;
it does not upload them or restart the API service. Deploy `published/files` as
the artifact root and `published/releases.json` as its catalog. The default
public artifact URL is `http://www.goldengeek.org/blip/releases`; override
`--base-url` only when the actual public hosting path differs.

## Local simulation and transport policy

The standard firmware and simulator use plain HTTP, following the user's
firmware-wide [private show-network policy](../../../docs/v2/adr/0020-private-show-network-defaults.md).
No API password, key, signing setup or certificate is required.

```powershell
python v2/tools/ota/release_simulator.py --listen 192.168.27.176
```

Use the PC's existing LAN address. The simulator never changes Wi-Fi. Its
catalog and artifacts stay under ignored `build/release-simulator/`. The empty
catalog returns unpublished; package a matching image with
`release_publish.py --base-url http://PC-LAN-IP:8088/blip/releases` to publish
candidates locally. Set `blip.updates.endpoint` to
`http://PC-LAN-IP:8088/blip/update` using serial or Advanced; endpoint and release
channel settings persist across reboots. All downloads retain bounded sizes,
whole-file SHA-256 checks and exact device/profile compatibility validation.

The production handler also runs without TLS:

```powershell
python v2/tools/ota/release_server.py --catalog releases.json --artifacts ./published --listen 127.0.0.1 --port 8088
```

The default device endpoint and artifact base use `http://www.goldengeek.org`.
The eventual host should serve these paths over HTTP without forcing an HTTPS
redirect for the standard build. The user will put the endpoint online later;
no remote deployment connection is required here.

HTTPS is optional: select `-DBLIP_ENABLE_RELEASE_TLS=ON` in a fresh build,
configure an HTTPS endpoint, and run the handler with both `--cert` and `--key`.
This build adds the common CA bundle, clock synchronization and TLS buffers.
It checks memory before connecting and reports deferred when the selected
component profile has insufficient free RAM. It keeps normal certificate,
hostname and time verification. The plain-HTTP standard build omits this cost.
SDK defaults do not overwrite an existing sdkconfig; the build reports an
explicit error when the HTTPS option and `CONFIG_ESP_HTTP_CLIENT_ENABLE_HTTPS`
disagree, rather than silently producing the wrong transport profile.

## Native firmware hardware check

After publishing a strictly newer matching release to the local simulator, run:

```powershell
python v2/tools/ota/blip_release_firmware_hil.py --port COM5 --mac 30:ae:a4:f2:d1:84 --endpoint http://PC-LAN-IP:8088/blip/update --build build/m6-production-huzzah32 --flash-log build/initial-flash.log --report build/native-firmware-update.json
```

This checks catalog eligibility, native download, one software reboot, boot
confirmation and retained name/interface, then restores the previous update
policy. It uses existing Wi-Fi and serial; it never changes the PC network.
The candidate build must have a greater `BLIP_RELEASE_SEQUENCE` than the installed
image. See the [checkpoint record](../../../docs/v2/work-packages/3.8-native-update-and-first-run.md)
for actual results and outstanding qualification.
