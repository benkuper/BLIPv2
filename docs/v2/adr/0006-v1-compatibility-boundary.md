# ADR-0006: V1 compatibility through explicit boundary adapters

- Status: Accepted
- Date: 2026-08-14
- Owners: BLIP V2 compatibility

## Context

V1 public behavior is useful to existing controllers and fleets, but its internal
architecture and formats are unversioned and sometimes unsafe. Copying V1 parser
and singleton behavior into V2 would make those constraints permanent. Removing
paths or formats without evidence would break users.

## Decision

V2 has canonical typed/versioned internal models. V1 compatibility exists only
in named boundary adapters and importers:

- `legacy_serial_v1` parses/emits V1 line grammar;
- `legacy_osc_v1` maps V1 paths/discovery/feedback to the transport envelope;
- `legacy_oscquery_v1` renders the registry in V1-compatible JSON;
- `legacy_settings_v1` imports the NVS MessagePack tree;
- `legacy_playback_v1` imports `.meta`/`.colors` pairs;
- `legacy_espnow_v1` participates only after explicit peer negotiation/detection.

Adapters validate all lengths/types before mapping to the canonical model. They
cannot expose raw pointers, native struct layouts, or V1 ownership rules to
components. Legacy output is opt-in by endpoint/profile or negotiated peer mode;
new peers receive only versioned V2 formats.

Every compatibility behavior has one of four dispositions in the compatibility
matrix: preserve, legacy adapter/import, intentionally not preserved, or pending
evidence. The default for a documented public path/format is preserve/adapter.
Removal requires explicit approval, a migration path, release notes, and at least
one deprecation cycle after telemetry/evidence is available.

Golden fixtures are immutable examples. If a fixture is wrong, a correction PR
must cite pinned source or device capture, retain the old sample as a rejected or
historical case when useful, and explain compatibility impact. Real captures are
sanitized and include provenance.

Unknown V1 behavior fails closed with a bounded diagnostic. V2 does not emulate
buffer overflows, overreads, non-atomic writes, build failures, or undocumented
singleton coupling.

## Public behavior at this decision

The authoritative dispositions are in
[`../compatibility.md`](../compatibility.md). No OSC path, legacy file format, or
peer mode is removed by this ADR. Empty RF24/Sequence and incomplete DC motor
code have no parity promise until user/device evidence establishes public use.

## Consequences

- Components and core remain independent of legacy wire/storage details.
- Compatibility adds flash and test cost and can be excluded from minimal
  profiles only when the profile says so explicitly.
- Mixed fleets can evolve without treating unversioned V1 bytes as the native V2
  protocol.
- Path changes require aliases rather than silent renames.

## Required tests

Every accepted fixture, malformed/truncated variants, maximum lengths, repeated
settings/playback import, V1/V1 reference behavior, V2/V2 behavior, negotiated
mixed-fleet behavior, alias resolution, and unsupported-format diagnostics are
required before the corresponding adapter ships.
