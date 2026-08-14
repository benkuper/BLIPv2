# blip_network

Owns the ESP-IDF Wi-Fi station/AP lifecycle and the bounded provisioning surface. The
component persists one versioned configuration through `storage.settings`, normalizes
imported V1 `/wifi` settings, exposes credentials as write-only control data, and starts
an open `BLIP-XXXXXX` SoftAP with a fixed-size HTTP form whenever credentials are absent
or a station connection times out.

Serial configuration reports success after radio application and NVS persistence.
SoftAP POST reports HTTP 202 after persistence, then applies the radio change after a
one-second response grace period. The AP remains available while station association is
pending and is removed after a station-only connection succeeds.

The persisted `antenna` parameter selects `board-default` (0), `onboard` (1), or
`external` (2). A board build must declare its RF-switch GPIOs before explicit
selection is accepted. `BLIP_BOARD_SEEED_XIAO_ESP32C6=ON` drives GPIO3 low to
enable that board's switch and uses GPIO14 low/high for onboard/U.FL selection.

Transport protocols that use the resulting IP service remain in their own components;
this component provides `transport.wifi` and `network.http`, owns the shared 2.4 GHz
Wi-Fi radio request, and runs the bounded 8 KiB port-80 server used by provisioning and
OSCQuery. While the public AP is active, plain `GET /` retains the setup form; OSCQuery
queries and WebSocket upgrades are delegated to the registered protocol component.

See [work package 2.6](../../../docs/v2/work-packages/2.6-wifi-provisioning.md) for
the settings format, state values, limits, verification, and rollback procedure.
