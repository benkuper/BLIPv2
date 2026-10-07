# ADR-0012: Checked UTF-8 guest strings

- Status: Accepted for milestone 6.4 implementation; provider bindings follow in 6.5
- Date: 2026-10-07
- Related: [ADR-0009](0009-wasm-service-boundary.md), [ADR-0011](0011-esp32-wasm-linear-arena.md)

## Decision

The version-1 string ABI is two unsigned Wasm i32 values: a byte offset into
default linear memory and a byte length. Offsets are guest addresses, never
native pointers. Strings are UTF-8 byte sequences and need no NUL terminator.
Embedded U+0000 is valid text; identifiers and individual providers may apply
their own narrower rules. No boundary may use `strlen` on guest input.

Copy at most 256 bytes per operation. Validate offsets with subtraction rather
than overflowing pointer-plus-length arithmetic, using the current memory
extent on every access. Empty strings still require an existing memory and
an offset at or before its end; an empty range at the end is valid. A pointer
beyond the end is invalid even when its length is zero.

The runtime-neutral memory boundary supplies checked copies into/out of
caller-owned buffers. It exposes no engine handle or borrowed native pointer.
The WAMR adapter alone queries the current memory instance/base/extent, checks
the range and performs the copy. Nonempty access without a memory base fails.
These APIs are worker-confined; guest threads and shared memory remain disabled.

String reads use a fixed 256-byte scratch buffer, validate exact UTF-8 bytes,
then copy to the caller. Failure sets the returned byte count to zero and
leaves caller output unchanged. Writes validate the complete host byte span
before checked memory copying. Invalid input or bounds must leave guest bytes
unchanged. Copies do not append a terminator or normalize Unicode.

Reject invalid continuation/lead bytes, truncation, overlong forms, surrogate
code points and values above U+10FFFF. A valid string ending at linear memory's
last byte is accepted. Byte length is independent of character count.

Service-level copies additionally require a loaded, runnable module and the
captured current module generation. Failed copies are diagnosed as explicit
status errors; they do not execute a guest, mark it faulted, allocate heap,
scan outside the declared span or retain a pointer through reload/growth.
Provider callbacks in 6.5 will use the same checked memory/string boundary.

## Verification

Host tests cover scalar boundaries, malformed/truncated UTF-8, embedded NUL,
empty and maximum-length spans, one-past-end ranges, wrapped offsets/lengths,
short destinations, failure atomicity, stale generations and lifecycle checks.
Native fixtures return pointer/length pairs from real guest code and verify
read/write round trips on ESP32, S3 and C6, including the ESP32 IRAM arena,
absent/zero/grown memory and repeated reload. Production family builds verify
the adapter integration. Provider imports and generated SDK remain later work.
