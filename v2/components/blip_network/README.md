# blip_network

Owns the ESP-IDF Wi-Fi station/AP lifecycle and the bounded provisioning surface. The
component persists one versioned configuration through `storage.settings`, normalizes
V2 Wi-Fi settings, exposes credentials as write-only control data, and starts
an open `BLIP-XXXXXX` SoftAP with a fixed-size HTTP form whenever credentials are absent
or a station connection times out.

Serial configuration reports success after radio application and NVS persistence.
SoftAP POST reports HTTP 202 after persistence, then applies the radio change after a
one-second response grace period. The AP remains available while station association is
pending and is removed after a station-only connection succeeds.

The persisted `antenna` parameter selects `board-default` (0), `onboard` (1), or
`external` (2). A board build must declare its RF-switch GPIOs before explicit
selection is accepted. The reference ESP32-C6 build selects the XIAO board automatically;
`BLIP_BOARD_SEEED_XIAO_ESP32C6=ON` remains available for an explicit profile. It drives GPIO3 low to
enable that board's switch and uses GPIO14 low/high for onboard/U.FL selection.
The `BLIP_BOARD_CREATORS_BALL_V2=ON` build has no RF-switch GPIOs; those pins
retain their Creators Ball LED and battery functions. A persisted `onboard`
antenna setting is accepted without driving any GPIO; `external` is rejected.

Transport protocols that use the resulting IP service remain in their own components;
this component provides `transport.wifi` and `network.http`, owns the shared 2.4 GHz
Wi-Fi radio request, and runs the bounded 8 KiB port-80 server used by provisioning and
OSCQuery. While the public AP is active, browser requests accepting `text/html` at
`GET /` retain the setup form. Other root requests, OSCQuery queries and WebSocket
upgrades are delegated to the registered protocol component.

The pinned Espressif mDNS responder publishes `_osc._udp` on port 9000 and
`_oscjson._tcp` on port 80 when the OSC component and HTTP server are running.
Both use the instance `BLIP V2 <12-digit STA MAC>` and hostname
`blip-<12-digit STA MAC>.local`. The responder follows STA/AP address changes,
withdraws records on server/OSC shutdown, and restarts with the shared server.
Disabled Wi-Fi and autonomous ESP-NOW operation do not advertise these services.
On C6, ordinary mDNS allocations prefer the byte-addressable RTC heap and fall
back to internal RAM, preserving DMA memory for Wi-Fi frames. The combined C6
BLE/WASM build reserves two static TX buffers at driver initialization and uses
four static RX buffers with a two-MSS TCP send window. This bounds the network
memory budget so large OSCQuery responses can progress alongside lighting input.

See [work package 2.6](../../../docs/v2/work-packages/2.6-wifi-provisioning.md) for
the settings format, state values, limits, verification, and rollback procedure.
