# ADR-0011: Separate guest linear memory on the original ESP32

- Status: Accepted for production profile compatibility; full Gate D remains open
- Date: 2026-10-07
- Related: [ADR-0008](0008-wasm-runtime-selection.md), [ADR-0010](0010-wasm-production-worker.md)

## Constraint

The original ESP32 production composition could not allocate the 80 KiB script
pool after starting Wi-Fi, storage, control, fleet and LED output. Those services
remain enabled. Reducing the guest memory below one Wasm page would prevent
ordinary one-page modules from loading.

## Decision

On ESP32, reserve a **16 KiB engine pool in DRAM** and a **64 KiB guest linear
arena in byte-accessible IRAM**. The module input buffer remains 16 KiB in DRAM.
The 4 KiB VM stack is inside the engine pool; both native task stacks also remain
in DRAM. The combined engine/linear reservation remains 80 KiB. C6 and S3 retain
the contiguous 80 KiB pool.

The ESP32 profile requires `CONFIG_FREERTOS_UNICORE=y` and
`CONFIG_ESP32_IRAM_AS_8BIT_ACCESSIBLE_MEMORY=y`. ESP-IDF's exception handler
emulates byte, half-word and unaligned integer access to that IRAM. Hardware
floating-point access cannot use it. The production build refuses an ESP32
configuration without these settings. This changes the original ESP32 profiles
to single-core scheduling; C6 and S3 scheduling is unchanged.

Only **guest linear memory** may use the IRAM arena. In the pinned, unmodified
WAMR fast interpreter (`25bd7eb63e828e4bd242cc9b38d260b4b31c6605`),
`wasm_loader.c` rewrites guest `f32`/`f64` memory loads and stores into integer
bit loads and stores. Floating-point computation and engine metadata remain in
DRAM. Changing the runtime revision or execution mode requires reviewing this
property and repeating the memory qualification. AOT, JIT, SIMD, shared memory,
guest threads and host imports remain disabled.

The runtime-neutral interface accepts an optional borrowed linear arena.
Backends must support it explicitly or reject it. WAMR requires an exactly
64 KiB, four-byte-aligned arena disjoint from its eight-byte-aligned engine
pool. Its exclusive global runtime owner configures the mapping state before
initialization and clears it after shutdown. A second mapping is refused;
growth cannot exceed one page. Reused and grown memory is zeroed, and unmapping
returns the arena to the owner without freeing the borrowed buffer.

The linear allocation uses `heap_caps_malloc`, whose four-byte base alignment
is sufficient for guest integer memory operations. Requesting extra alignment
made TLSF skip the available 64 KiB size class, despite sufficient total free
IRAM. Engine allocation retains explicit eight-byte alignment.

## Accounting and verification

Reserved and currently used bytes include both arenas. For separate arenas,
`pool_peak` is the sum of their individual high-water marks: a conservative
upper bound, which may exceed the maximum simultaneous use. Unload must return
reported usage to zero. Component stop releases both buffers only after native
worker quiescence and joining.

The production HIL fixtures cover data initialization, aligned and unaligned
floating-point bit storage, actual floating-point arithmetic, NaN payloads,
signed byte/odd half-word operations, the last valid byte, overlapping bulk
copy/fill, a cross-boundary trap, zero-to-one-page growth, growth refusal and
zeroing on reload. The profile matrix also covers fault recovery, cancellation,
overload, stale generations, completion expiry and repeated load/unload.

Byte access through exception handling has a performance cost. This decision
establishes bounded memory and profile compatibility, not a guest-memory
throughput guarantee. Native task failure/restart qualification, active fleet
and BLE coexistence, longer soaks and physical LED timing remain required for
full Gate D.
