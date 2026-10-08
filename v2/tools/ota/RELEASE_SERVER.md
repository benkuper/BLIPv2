# GoldenGeek release endpoint

`release_server.py` is a dependency-free Python HTTPS handler for
`/blip/update` and optional catalog-listed `/blip/releases/` artifacts. It is
ready to connect to a host; this repository does not deploy it to GoldenGeek.
Contract: [ADR-0019](../../../docs/v2/adr/0019-device-release-catalog.md).

Run behind a reverse proxy that preserves the original GET query, or terminate
TLS directly with a valid certificate for the device-visible hostname:

```powershell
python v2/tools/ota/release_server.py --catalog releases.json --artifacts ./published --cert fullchain.pem --key privkey.pem
```

The listener defaults to `127.0.0.1:8443`. There is no insecure HTTP startup mode.
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
the production server entry point requires TLS.
