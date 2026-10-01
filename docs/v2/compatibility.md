# V1 to V2 public compatibility contract

This contract is anchored to V1 commit
`e567eeb5f20ba022595fd5b89a77fb17ba59ca94`. No public V1 path or file format is dropped by this milestone. This matrix sets
the default disposition for later implementation. A change from **preserve** or
**legacy adapter/import** to removal requires explicit approval and a new ADR.

## Compatibility matrix

| Surface | V1 observable contract | V2 disposition | Verification |
| --- | --- | --- | --- |
| Component paths | Slash hierarchy for OSC/OSCQuery, dot hierarchy for incoming serial | **Preserve** stable component, parameter, trigger, and event names for ported components; add aliases if the V2 canonical name differs | registry descriptor snapshots plus V1 OSCQuery fixtures |
| Root controls | `shutdown`, `restart`, `standby`, `stats`, `log`; conditional radio switches | **Preserve** safe controls; diagnostics may gain structured equivalents | command adapter tests |
| Serial discovery | input `yo`; output `wassup <id> "<type>" "<name>" "<version>"` | **Preserve** for legacy host tools | raw line fixtures |
| Serial control | `<component.path>.<command> [comma-separated arguments]` | **Legacy adapter** into the versioned V2 transport envelope | tokenizer and routing fixtures |
| Serial feedback | `<source>.<name> <space-separated values>`; V1 source may be `/`-prefixed and underscore-separated | **Legacy adapter**; preserve exact legacy mode, use canonical slash paths in V2 mode | output line fixtures |
| OSC discovery | `/yo <host>` -> `/wassup <ip> <id> <type> <name> <version>` | **Preserve** on UDP | binary OSC fixtures |
| OSC liveness | `/ping [host]` -> `/pong <id>` | **Preserve** | binary OSC fixtures |
| OSC controls | `/<component path>/<command>` with up to eight useful arguments | **Preserve** valid path and value behavior; reject malformed/oversized messages safely | binary OSC plus negative tests |
| OSC feedback | same path, first UDP argument is device ID; WebSocket feedback omits device ID | **Preserve** through transport-specific compatibility options | transport fixture tests |
| OSCQuery host info | `GET /?HOST_INFO`, uppercase OSCQuery keys, UDP port 9000 | **Preserve** required keys and extension semantics | JSON fixture |
| OSCQuery tree | `GET /`; nodes contain `DESCRIPTION`, `FULL_PATH`, `ACCESS`, `CONTENTS`; values include `TYPE`, `VALUE`, optional `RANGE` | **Preserve** for legacy endpoint, generated solely from the V2 registry | JSON schema/snapshot fixture |
| OSCQuery config filter | `GET /?config=0` omits V1 `Config` parameters | **Preserve** behavior | filtered snapshot test |
| WebSocket control | binary OSC on WebSocket `/`; binary OSC feedback; JSON text debug messages | **Preserve** in compatibility endpoint; versioned V2 WebSocket envelope may coexist | frame fixtures |
| HTTP files | `/firstrun`, `/uploadFile`, filesystem static assets, gzip fallback | **Preserve capability**, not necessarily implementation details; V2 must authenticate or explicitly document local trust policy before production | HTTP contract tests |
| Settings | one ArduinoJson MessagePack object in NVS namespace `blip`, key `settings`; nested `components` maps | **Import once** into versioned per-component V2 settings; retain source blob until confirmed migration | MessagePack/JSON fixture and idempotent migration test |
| Wi-Fi credentials | strings inside V1 settings tree | **Import with redaction-aware diagnostics**; never echo passwords | synthetic settings fixture |
| Filesystem paths | `/scripts`, `/playback`, `/server`; paired playback files | **Legacy import/read**; V2 writes only a versioned format | playback/file fixtures |
| Playback metadata | JSON `.meta` with `fps`, `group`, `id`, `groupColor`, optional `scripts` | **Import** with strict bounds and defaults | `.meta` fixture |
| Playback pixels | raw `.colors`; active profile uses 8-bit bytes in A,R,G,B order, frame size `pixel_count * 4` | **Import**; V2 native format declares version, format, dimensions, and checksum | `.colors` fixture and golden decoded frames |
| Art-Net/DMX mapping | universes and one-based channels map to RGB/RGBA, optional 16-bit values | **Preserve** in optional components | packet/channel vector tests |
| ESP-NOW control | unversioned type byte plus native-sized values | **Excluded**; V2-only packets are versioned and bounded | V2 peer, loss, reorder, and duplicate tests |
| ESP-NOW stream | type 1, big-endian universe/start channel, raw RGB triples | **Excluded**; V2 streaming uses its own transport design | V1 fixture retained only as historical evidence |
| ESP-NOW pairing/wake | types 2, 3, and 4 | **Excluded**; V2 pairing and wake require their own design | V1 fixture retained only as historical evidence |
| WASM scripts | wasm3 module plus central native function bindings and dynamic events/parameters | **Source/API migration**, not binary ABI promise; capability manifest is versioned | later WASM SDK fixtures |
| Empty/incomplete components | Sequence, RF24 placeholder, incomplete DC motor | **No parity claim** until a public use/capture is supplied; do not reproduce build breakage | issue evidence record |
| Parser bugs/unsafe behavior | overreads, unbounded assumptions, non-atomic save, tokenizer quirks | **Intentionally not preserved** except benign lexical quirks in explicit legacy mode | fuzz and fault-injection tests |

## Exact V1 formats

### Settings object

V1 saves the root object's configuration parameters and a recursively nested
`components` object. A representative shape is:

```json
{
  "components": {
    "comm": {
      "enabled": true,
      "components": {
        "serial": { "enabled": true, "sendFeedback": true },
        "osc": { "enabled": true, "remoteHost": "192.0.2.10" }
      }
    },
    "settings": { "enabled": true, "propID": 7, "deviceName": "Fixture BLIP" }
  }
}
```

It is encoded as MessagePack and stored with `Preferences.begin("blip")` and
`putBytes("settings", ...)`. There is no magic, version, checksum, length field
inside the blob, generation counter, or two-slot commit record.

### Serial grammar

Input is one newline-terminated record:

```text
yo
settings.save
leds.strip1.brightness 0.5
leds.strip1.playbackLayer.play demo,1.25
```

The target is split at its last dot. No dot means the root component. Arguments
are split only at commas, with at most eight delivered values. A token containing
only digits and at most one dot becomes an integer or float; negative values,
whitespace-padded values, and scientific notation remain strings. There is no
escaping or quoting in normal commands.

### OSC and OSCQuery

Normal OSC control paths are converted by replacing `/` with `.` and routing the
last segment as the command. OSC strings are read into fixed 32-byte buffers and
addresses into a fixed 64-byte buffer in V1; these limits are fixture facts, not
V2 parser limits. Supported V1 value types are string, int32, float32, boolean,
RGBA, MIDI, and timetag.

OSCQuery access values are `0` for container nodes, `1` for read-only feedback,
and `3` for writable parameters/actions. Parameter type strings are `b`, `i`,
`f`, `s`, `ff`, `fff`, and `r`; booleans are advertised as `T` or `F` according
to their current value. Enum parameters are advertised as string type `s` with
`RANGE: [{"VALS": [...]}]`.

### ESP-NOW packets

All V1 ESP-NOW application payloads are at most 250 bytes and begin with a type:

| Type | Layout |
| --- | --- |
| `0` message | `type:u8`, `start_id:u8`, `end_id:u8`, `address_len:u8`, address bytes, `command_len:u8`, command bytes, then typed values |
| `1` stream | `type:u8`, `universe:u16be`, `start_channel:u16be`, RGB byte triples |
| `2` pairing/ping | `type:u8`, `wifi_channel:u8` |
| `3` pairing response | `type:u8` |
| `4` remote wake | `type:u8`, `wifi_channel:u8` |

Each message value starts with an ASCII type byte. `b` is one byte. `i` and `f`
are four-byte native ESP32 values (little-endian int32/IEEE-754 float32 in the
supported V1 builds). `s` and `p` add a one-byte length then that many bytes.
IDs `255..255` mean all devices. Addresses are sent slash-delimited and converted
to dots at receive time.

The V2 radio protocol must start with an unambiguous magic/version/header and
must use explicit byte order and sizes. Mixed-fleet logic may parse the legacy
layout only after length validation.

### Playback pair

For a logical name `demo`, V1 opens `/playback/demo.meta` and
`/playback/demo.colors`. Metadata fields used by the pinned reader are:

```json
{
  "fps": 30,
  "group": 1,
  "id": 2,
  "groupColor": [1.0, 0.25, 0.0],
  "scripts": [{ "name": "intro", "start": 0.0, "end": 1.5 }]
}
```

The color file has no header. With `PLAYBACK_USE_ALPHA` and 8-bit `ColorType`,
each pixel is four bytes in native struct order **A, R, G, B**. Frames are simply
concatenated. Pixel count comes from the current strip configuration; total frame
count is `file_size / (pixel_count * 4)`. The reader does not reject a trailing
partial frame when computing metadata, so the V2 importer must.

## Fixture coverage

[`v2/tests/fixtures/v1/manifest.json`](../../v2/tests/fixtures/v1/manifest.json)
maps every fixture to its format, expected interpretation, evidence class, and
source paths. Coverage includes:

- a representative settings object in JSON and MessagePack;
- a complete representative `creatorsballv2` OSCQuery tree and host info;
- raw serial input/output records;
- raw OSC datagrams;
- all five ESP-NOW packet families plus typed message values;
- paired playback metadata/colors and decoded golden frames.

Fixture content is synthetic and safe to publish. Real device captures must be
added with secrets removed and a capture record naming firmware commit, build
environment, board revision, and capture tool version.
