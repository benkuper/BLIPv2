# BLIP V2 development

This repository is the standalone development workspace for the
[BLIP V2 implementation plan](Plan.md). The completed Milestone 0 foundation is
indexed in [`docs/v2/`](docs/v2/) and anchored to BLIP V1 commit
`e567eeb5f20ba022595fd5b89a77fb17ba59ca94`.

The current V2 line includes the reproducible ESP-IDF/C++20 foundation, storage,
diagnostics and safe mode, Serial/USB control, Wi-Fi provisioning, OSC/OSCQuery,
and a schema-driven browser control shell. See the
[work-package index](docs/v2/work-packages/README.md) and the
[dependency map](docs/v2/dependency-map.md).

Run the offline foundation check with:

```powershell
python v2/tests/host/validate_foundation.py
python v2/tests/host/validate_bootstrap.py
python v2/tests/host/validate_resource_manifests.py
```

Configure and run the portable C++ host suite with CMake/CTest from
`v2/tests/host`.
