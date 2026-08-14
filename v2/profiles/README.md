# Build and target profiles

Target manifests in `targets/` define the initial ESP-IDF compilation/validation
matrix. Feature and product profiles will be added through the Firmware Kitchen
schema in work package 8.1.

Build-time resource inventories and feature claims validate against
`resource-manifest.schema.json`. The `features/minimal.json` profile claims no
hardware; conflict fixtures exercise deterministic diagnostics before firmware
configuration begins.

Validate them offline with:

```powershell
python v2/tests/host/validate_foundation.py
python v2/tests/host/validate_resource_manifests.py
```
