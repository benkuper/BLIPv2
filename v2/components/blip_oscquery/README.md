# blip_oscquery

Owns the bounded OSC compatibility endpoint and registry-generated OSCQuery
discovery surface. UDP OSC listens on port 9000. The Wi-Fi component owns the
shared port-80 HTTP server and delegates OSCQuery requests and WebSocket frames
to this component.

The codec accepts one OSC message of at most 1,024 bytes, a 128-byte address,
256-byte strings, and eight useful control arguments. It supports boolean,
int32, float32, string, RGBA, MIDI, and timetag values, validates padding and
UTF-8, and rejects bundles, truncation, trailing bytes, non-finite numbers, and
unsupported tags. It does not allocate from the heap while decoding, encoding,
or routing a message.

`GET /?HOST_INFO`, `GET /?config=1`, and `GET /?config=0` stream JSON directly
from the live component registry. The filtered tree omits persisted
configuration parameters. Binary WebSocket frames carry OSC; text frames return
a bounded JSON debug acknowledgement. UDP control feedback prefixes the device
ID, while WebSocket control feedback does not. `/yo`/`/wassup` discovery and
`/ping`/`/pong` liveness preserve the V1 behavior.

Components may declare typed legacy parameter aliases in their descriptors. The
adapter uses those declarations for both control routing and discovery, including
enum label conversion. Wi-Fi therefore exposes the preserved `pass`, `manualIP`,
`manualGateway`, `channelScanMode`, `txPower`, and `wifiProtocol` names without
duplicating control logic. Password nodes never include `VALUE`.

Work package 3.1 adds optional `BLIP_*` annotations to the registry-derived tree
for generic web controls. These identify component/control kinds, action and
event fields, parameter persistence and access semantics, numeric steps, units,
component schema versions, and disable policy. Existing OSCQuery fields and
public OSC paths remain unchanged.

The endpoints are unauthenticated local-network compatibility services over
plain UDP, HTTP, and WebSocket. Deployments must treat the attached LAN as trusted
or add an authenticated gateway before exposing them beyond it.

See [work package 2.7](../../../docs/v2/work-packages/2.7-osc-oscquery.md) for the
wire limits, V1 compatibility details, verification evidence, and rollback.
The additive web-schema contract is recorded in
[work package 3.1](../../../docs/v2/work-packages/3.1-schema-web-shell.md).
