# BLIP V2

This tree contains the ESP-IDF/C++20 firmware, reusable components,
profiles, web application, build tools, and compatibility tests described in
[`docs/v2/`](../docs/v2/). Milestone 0 established the contracts and fixtures.
Milestones 1 and 2 provide the reproducible firmware core, storage, diagnostics,
Serial/USB, Wi-Fi, and OSC/OSCQuery. Milestone 3 now begins with a browser shell
whose controls are generated exclusively from the live registry schema.

The legacy Arduino source is not vendored in this standalone development
repository. Compatibility evidence is anchored to its immutable upstream commit
and can be inspected without merging the histories.
