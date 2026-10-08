# ADR-0016: Owned script values, queues and callback buffers

- Date: 2026-10-08
- Status: Store and passive runtime query qualified; production integration pending
- Extends: [ADR-0015](0015-wasm-script-control-declarations.md)
- Related: [ADR-0010](0010-wasm-production-worker.md), [ADR-0012](0012-wasm-checked-utf8.md)
- Evidence: [owned script store](../evidence/wasm/2026-10-08-script-control-store.json)

## Owner and publication

`ScriptControlStore` owns the immutable declaration, mutable parameter values,
four copied action messages, four copied event messages and an invocation-scoped
event builder. It performs no heap allocation. Metadata projections borrow the
owner under a move-only core schema lease. Values and queued strings are copied;
no guest or uploaded-module pointer survives an operation.

Only the worker prepares and publishes a schema. Preparation runs before engine
load because WAMR may rewrite its mutable module buffer. It validates declaration
framing and collisions with the owner's static parameters/actions/events and
legacy aliases. Publication separately validates every callback signature and,
when required, the guest string arena, then initializes all parameter defaults
before making the new schema generation visible. Metadata decoding is not engine
validation; both phases must succeed before production publication.

Schema tokens increase monotonically for the store's lifetime and are distinct
from module generations and the worker cancellation epoch. Tokens never reset or
wrap. Retirement immediately closes admission. Existing leases continue reading
the old owned metadata, but old value/action requests are refused. Replacement
cannot overwrite metadata until every lease releases. Writers, value operations
and queue operations attempt admission immediately; schema acquisition makes at
most four atomic attempts and returns a busy error on contention.

Retirement preserves queued action messages and their original request IDs. The
worker must complete those IDs with cancellation before replacing the schema;
preparation rejects a nonempty action queue. Queued events carry schema/module
generations, and polling discards retired generations. The lifecycle owner also
waits for its worker and all metadata leases before freeing the store. A lease
does not protect an independently destroyed component.

## Values and callback ABI

Strings are at most 128 UTF-8 bytes, may contain NUL, and retain explicit lengths.
Integers retain all signed i64 bits. Numbers must be finite. External reads and
writes enforce declared access, type and bounds. The owning guest may update a
read-only external status parameter or inspect a write-only external setting;
type, bounds, string capacity and UTF-8 validation still apply.

An action callback exports a function with no results. Required fields expand
in declaration order:

| Field | Wasm callback arguments |
| --- | --- |
| Boolean | One i32, 0 or 1 |
| Integer | One i64, preserving signed bits |
| Number | One f64 |
| String | Two i32 values: unsigned linear-memory offset and byte length |

Four string fields therefore fit the eight-argument limit. An action containing
any string field must export an **immutable i32 global** named
`blip_controls_buffer_v1`. Its unsigned value identifies a writable 512-byte
scratch arena in the admitted module's linear memory. Field `i` uses the
128-byte slice at `base + i * 128`. The worker copies each queued string through
the checked guest-memory API before the supervised callback. The guest owns this
arena and keeps it separate from data it needs to retain across callbacks. There
is no allocator or unbudgeted helper export to obtain a buffer.

The runtime exposes a passive `immutable_i32_global` query. Missing, mutable,
non-i32 and unloaded globals are rejected. It copies the i32 value and exposes no
engine pointer. Publication checks arithmetic and the entire fixed arena extent
before admitting the declaration. Querying signatures/globals executes no guest
code. Backends without this query refuse string action publication explicitly.

## Queues and events

Actions copy their callback name, typed fields, schema/module generation,
cancellation epoch and caller-assigned nonzero request ID into a fixed FIFO.
Overflow rejects the new action. The lifecycle owner assigns globally unique
IDs, captures execution budgets at admission and invokes only on its supervised
worker. The store alone neither executes callbacks nor publishes completions.

The event builder accepts one declared event at a time and returns a monotonic,
nonzero token. Every field must be supplied with its declared type before commit.
Boolean bits, finite numbers and copied UTF-8 receive the same validation as
values. Successful commit transfers an owned payload to the four-message FIFO;
overflow rejects the new event. Ending an invocation discards an incomplete
builder. Tokens and retired payloads cannot address a replacement module.

## Qualification boundary

The shared tests exercise a real reader thread holding metadata during retirement
and rejected replacement. A standalone qualifier also loads an actual WAMR
module, validates globals, copies string fields and executes its typed callback.
It does not instantiate a production script owner or exercise simultaneous
HTTP/WebSocket queries and module reload. Guest value/event imports, production
load/fault/stop wiring, external event delivery and automatic web schema refresh
remain required. This ADR adds a storage/callback contract without changing the
declaration bytes defined by ADR-0015.
