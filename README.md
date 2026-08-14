# BLIP V2 development

This repository is the standalone development workspace for the
[BLIP V2 implementation plan](Plan.md). The completed Milestone 0 foundation is
indexed in [`docs/v2/`](docs/v2/) and anchored to BLIP V1 commit
`e567eeb5f20ba022595fd5b89a77fb17ba59ca94`.

There is intentionally no production V2 firmware yet. Work package 1.1 begins
the ESP-IDF/C++20 application after these contracts and fixtures are reviewed.

Run the offline foundation check with:

```powershell
python v2/tests/host/validate_foundation.py
```
