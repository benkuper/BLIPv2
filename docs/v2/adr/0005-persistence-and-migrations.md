# ADR-0005: Versioned transactional persistence and migrations

- Status: Accepted
- Date: 2026-08-14
- Owners: BLIP V2 storage

## Context

V1 stores one unversioned MessagePack tree by clearing an NVS namespace and then
writing a replacement. Playback and other files are unversioned pairs without
checksums. Power loss can leave missing/corrupt state, and a monolithic settings
blob makes independent component evolution difficult.

## Decision

Each persisted record starts with a format magic, format version, component or
record type ID, schema version, payload length, generation, and integrity check.
Multi-byte fields have explicit byte order. The registry descriptor binds each
component to its current settings schema and migration functions.

NVS settings use two data slots plus a small commit record:

1. Read and validate both slots and the commit record.
2. Choose the highest valid committed generation, falling back to the other
   valid slot if the commit record is torn/corrupt.
3. Serialize the new generation into the inactive slot.
4. Read back and verify header, length, checksum, and schema.
5. Atomically replace/update the commit record to select it.
6. Keep the previous valid slot until a later successful commit.

Unknown fields are retained where the encoding permits, or preserved in an
opaque extension map, so downgrade/upgrade does not silently erase settings.
Settings validation happens before start and reports field-level errors. A
component never reads NVS directly.

Files are written to a same-filesystem temporary name, flushed, verified, and
atomically renamed where supported. Where the filesystem cannot promise atomic
rename, the storage service uses versioned names plus a checksummed pointer
record. Multi-file objects use a manifest committed last. Readers never infer a
format from an extension alone.

Migrations are pure, ordered, and idempotent transformations from one schema
version to the next. The original source record/blob is retained until the new
record has booted successfully and migration confirmation is committed. Migration
failure enters a diagnosable safe/default state without destroying the source.

The V1 importer recognizes only the documented NVS MessagePack and playback
fixtures. It applies strict size/depth/type/range limits, records source hash and
importer version, and never writes V1 format.

## Consequences

- Interrupted saves yield the previous or new valid state.
- Storage use is higher because two generations and source retention coexist.
- Every schema change requires migration and downgrade notes.
- File services, not components, own flush/verify/commit behavior.

## Required tests

Power interruption at every write/erase/commit boundary, corrupt header/length/
checksum, two valid slots, torn commit, NVS full, unknown fields, every migration
hop, repeated import, downgrade, partial multi-file update, and all golden V1
fixtures are mandatory.

