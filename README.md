# BLIP V2 development

This repository is the standalone development workspace for the
[BLIP V2 implementation plan](Plan.md). The completed Milestone 0 foundation is
indexed in [`docs/v2/`](docs/v2/) and anchored to BLIP V1 commit
`e567eeb5f20ba022595fd5b89a77fb17ba59ca94`.

The ESP-IDF/C++20 Milestone 1 core now includes the reproducible bootstrap,
component registry/lifecycle, typed descriptors, resource broker, scheduler,
and bounded event bus. See the
[verification record](docs/v2/work-packages/milestone-1-evidence.md). Production
features remain gated by the dependency map and Gate A still requires physical
board evidence.

Run the offline foundation check with:

```powershell
python v2/tests/host/validate_foundation.py
python v2/tests/host/validate_bootstrap.py
python v2/tests/host/validate_resource_manifests.py
```

Configure and run the portable C++ host suite with CMake/CTest from
`v2/tests/host`.
