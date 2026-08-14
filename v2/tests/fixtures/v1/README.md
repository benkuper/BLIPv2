# V1 compatibility fixtures

These fixtures are deterministic, synthetic, and source-derived from BLIP V1
commit `e567eeb5f20ba022595fd5b89a77fb17ba59ca94`. They contain no device capture,
real credential, or private user content.

`manifest.json` records the SHA-256 digest, format, source provenance, and expected
interpretation of every generated artifact. Binary files are checked in so host
tests can consume them without Arduino, ESP-IDF, network access, or hardware.

Regenerate after an intentional fixture-source change:

```powershell
python v2/tests/fixtures/v1/generate.py
```

Verify without changing the working tree:

```powershell
python v2/tests/fixtures/v1/generate.py --check
python v2/tests/host/validate_foundation.py
```

Do not hand-edit generated artifacts. A correction must cite either the pinned
source lines or a sanitized device capture with board/build provenance.

