# ADR-0015: Owned script control declarations

- Date: 2026-10-08
- Status: Declaration format implemented; live publication and dispatch pending
- Related: [ADR-0001](0001-component-lifecycle-and-registry.md),
  [ADR-0010](0010-wasm-production-worker.md), [ADR-0012](0012-wasm-checked-utf8.md)

## Declaration boundary

A script carries its parameter, action and event declarations in the optional
`blip.controls.v1` WebAssembly custom section. The section contains metadata;
decoding it executes no guest code. Custom sections carry a name and custom
bytes independently of WebAssembly execution semantics. See the
[WebAssembly binary specification](https://webassembly.github.io/spec/core/binary/modules.html#custom-section).

Schemas are immutable for a loaded generation. Runtime changes to parameter
values and event payloads are separate from schema declarations. The firmware
copies all schema text into fixed owned storage before it can be published.
No descriptor retains an uploaded-module or guest-memory pointer. Copies and
moves of the owned schema remain valid because internal references are offsets.
Descriptor projections borrow the schema's text, and action/event projections
also borrow their caller-provided field arrays; owners keep both alive.

One module may declare at most 16 controls, with at most four required fields
per action/event. Names are unique across all three control kinds. Field names
are unique within their control. IDs follow the registry's lowercase public-ID
grammar: `[a-z][a-z0-9_-]*`, with a maximum of 32 bytes. Labels are nonempty
UTF-8, at most 64 bytes. Units may be empty and are at most 16 bytes.
Display metadata excludes ASCII control bytes and DEL. String defaults are
checked UTF-8 of at most 128 bytes and may include embedded U+0000.

The declaration's total custom-section payload, including its name encoding,
is at most 4,096 bytes. The enclosing module is at most 16 KiB. All owned text,
including field names, defaults and action export names, shares a 2,048-byte
arena. Exhausting any limit rejects the declaration; there is no heap fallback.
The complete owned schema is at most 4 KiB. Its owner must reserve it outside
the production worker's 8 KiB stack.

## Version 1 binary layout

WebAssembly's outer section ID is zero. Section/name lengths use unsigned
LEB128 `u32`, with the usual five-byte/32-bit bounds. Unknown custom sections
are skipped after valid UTF-8 name/framing checks. A second declaration section
is an error. Any other `blip.controls.*` name, including a NUL suffix or
noncanonical version spelling, is an incompatible version rather than an alias.

After the name, the declaration data is:

1. Four bytes `BCM1`, then a `u32` control count.
2. Each control: one kind byte, ID text, label text, then its kind-specific body.
3. No trailing declaration bytes.

Text is a `u32` byte count followed by exactly that many UTF-8 bytes, without
an implicit terminator. Kind bytes are parameter=0, action=1, event=2.
Scalar type bytes are boolean=0, integer=1, number=2, string=3.

| Kind | Body |
| --- | --- |
| Parameter | Type byte, access byte, unit text, typed default, bounds flag, optional bounds |
| Action | Callback export text, `u32` field count, repeated field ID text/type byte |
| Event | `u32` field count, repeated field ID text/type byte |

Access bytes are read-only=0, write-only=1, read/write=2. A boolean default is
one byte, exactly zero or one. Integer defaults use eight little-endian bytes
of signed i64 bits. Number defaults use eight little-endian IEEE-754 f64 bits;
nonfinite values are rejected, and signed zero is preserved. String defaults
use the text encoding above. Parameters are transient, not persisted.

The bounds flag is exactly zero or one. Bounds apply only to integer/number
parameters and contain three finite little-endian f64 values: minimum, maximum,
step. Minimum must not exceed maximum, step is positive, and the default is in
range. Integer bounds/step must be integral and within
`[-(2^53-1), +(2^53-1)]`, avoiding lossy comparisons through double. Unbounded
integer defaults retain the full i64 range. Step is UI metadata; the declaration
does not require the default to lie on a step grid.

Action export names follow `[A-Za-z_][A-Za-z0-9_]*`, at most 64 bytes. The metadata
parser validates their spelling. Presence, signature and passive-module policy
remain separate engine/loading checks before an action can become callable.

## Failure and publication requirements

The input must remain stable throughout decoding and must not overlap the output
object. The decoder first validates that entire input, then copies into the
destination in a second pass. Rejected input leaves the destination byte-for-byte
unchanged. It uses bounded local readers, ID views and one record, with no
allocation or full-schema stack temporary. Diagnostics use static text and do
not borrow rejected metadata. This is a bounded metadata parser, not a complete
WebAssembly validator; successful decoding alone does not admit execution.

Live integration must publish a complete validated generation through the
registry. OSCQuery and web consume that same schema; separate hand-written
panels/catalogs are prohibited. Readers must have a stable snapshot or lease
while serializing, rather than walking mutable borrowed descriptors. Module
failure, unload and replacement retire its callable controls and queued work.
Control requests and event payloads must own their data and carry generation
tokens, so old work cannot call a replacement module. Guest action execution
belongs on the supervised worker and must not run inline on transport tasks or
re-enter the guest from native callbacks.

The declaration checkpoint implements decoding and descriptor projection. The
subsequent [registry interface checkpoint](../work-packages/6.6-dynamic-registry-interfaces.md)
adds component-owned generation leases, checked dynamic dispatch, owned string
replies and OSCQuery/OSC projection. Its native owner is a single-threaded
surrogate; production loading does not yet consume or publish this section.
Reader-safe schema replacement/retirement, parameter value storage, supervised
guest action dispatch, event delivery, external generation tokens, generated
bindings and their lifecycle/concurrency evidence remain required for 6.6/6.7.
