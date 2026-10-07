# ADR-0008: Select the metered WAMR interpreter

- Status: Accepted for Milestone 6 runtime selection; production Gate D remains open
- Date: 2026-10-07
- Owners: BLIP V2 core

## Decision

Use WAMR 2.4.5's fast interpreter behind the runtime-neutral WASM service.
Keep instruction metering, software memory bounds checks and a declared memory
pool. Disable AOT/JIT, WASI, guest threads and filesystem/network access. Component
providers must not depend on WAMR types. Do not import V1's script ABI.

The benchmark does not disqualify the Plan's preferred runtime. wasm3 is faster
for several workloads, but the released engine does not terminate a tight branch
loop through its yield hook. On all three chip families, the independent test
supervisor had to reset the board. This fails the required script isolation
behavior. A patched wasm3 fork could add loop metering, but maintaining that fork
is unnecessary when WAMR provides the needed interpreter limit.

## Measurements

The [immutable-input harness](../../../v2/qualification/wasm/README.md) compared
native baseline, WAMR and wasm3 on XIAO C6 (160 MHz), M5Dial S3 (240 MHz), and
HUZZAH32 ESP32 (240 MHz). The [nine-run hardware evidence](../evidence/wasm/2026-10-07-runtime-benchmark.json)
binds each result to its flashed image, ELF/map/config hashes and linked size.
All runs used harness SHA-256
`8676a698122c2585483922b559a9008b4b304cbef6d4bfa28efb4fa1ebe6a64e`.

### Linked cost

Bytes relative to the same chip's native baseline. DIRAM includes both code and
data on C6/S3; do not interpret its entire delta as static data alone. Full section
breakdowns are in the evidence. Pools and task stacks are additional boot heap
reservations, not these static section deltas.

| Chip | Engine | App bytes | App delta | DRAM/DIRAM delta | Separate IRAM delta |
| --- | --- | ---: | ---: | ---: | ---: |
| ESP32 | WAMR | 192,928 | 67,200 | 1,192 DRAM | 148 |
| ESP32 | wasm3 | 197,904 | 72,176 | 56 DRAM | 148 |
| ESP32-S3 | WAMR | 210,960 | 67,328 | 1,332 DIRAM | 0 |
| ESP32-S3 | wasm3 | 215,648 | 72,016 | 196 DIRAM | 0 |
| ESP32-C6 | WAMR | 202,512 | 74,592 | 1,496 DIRAM | n/a |
| ESP32-C6 | wasm3 | 212,080 | 84,160 | 206 DIRAM | n/a |

WAMR reserved a 131,072-byte pool; used and peak pool bytes were **76,992** on
every target, including one 64 KiB linear memory and an 8 KiB Wasm stack. Its SDK
heap drop at load was 131,300 bytes on C6/S3 and 131,308 on ESP32. wasm3's ordinary
heap drop was 110,360 / 113,012 / 115,080 bytes on C6 / S3 / ESP32. Pool reservation
and used bytes describe different costs; neither is interchangeable with the
other engine's allocation policy.

Both variants declared a 32,768-byte native pthread stack. WAMR's measured native
stack headroom was 30,800 / 30,452 / 30,468 bytes on C6 / S3 / ESP32; wasm3's was
23,732 / 23,460 / 23,684. These results suggest a smaller WAMR production stack
may be possible, but do not qualify one. WAMR cleanup left a 112-byte heap delta
on C6/S3 and 116 on ESP32. Repeated load/unload and attribution of this retained
memory are required before claiming a leak-free production lifecycle. wasm3
cleanup after its infinite loop was not measurable without resetting the board.

### Execution speed

Median microseconds per call over 21 samples after warmup. Noop uses batches of
100 calls, divided here by 100. Integer performs 4,096 iterations; float 2,048;
pixels generates/stores 256 RGB values; host calls invokes a native import 512
times. Every sample's result was checked. Native reference results and all
min/max/load timings are retained in the evidence.

| Chip | Engine | Noop | Integer | Float | Pixels | Host calls |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| ESP32 | WAMR | 22.24 | 5,898 | 4,164 | 969 | 1,947 |
| ESP32 | wasm3 | 2.51 | 4,366 | 5,581 | 1,304 | 911 |
| ESP32-S3 | WAMR | 8.81 | 4,742 | 3,341 | 769 | 1,550 |
| ESP32-S3 | wasm3 | 2.09 | 3,744 | 5,091 | 1,101 | 764 |
| ESP32-C6 | WAMR | 7.96 | 6,051 | 6,863 | 1,023 | 2,249 |
| ESP32-C6 | wasm3 | 2.73 | 2,630 | 7,299 | 542 | 722 |

WAMR is not universally faster. Its metered 256-pixel workload completes in
0.77–1.03 ms here, and its code/used-memory costs fit these standalone targets.
The full production radio/LED profile must independently prove sufficient RAM
and scheduling headroom. These workloads do not bound every opcode or native
provider and do not establish a wall-clock deadline for arbitrary guest code.

### Fault handling

Both engines passed zero-initialized memory, rejected growth beyond one page,
unreachable, out-of-bounds load, divide-by-zero and recursive stack overflow.
A valid call succeeded after each trap. No semantic result mismatches occurred.

WAMR's 10,000-instruction tight loop terminated and recovered in **3,880 us on
ESP32, 3,019 us on S3 and 3,680 us on C6**. wasm3 did not return; the supervisor
reset after 106,675 / 100,745 / 107,895 us respectively. A board reset is a failed
script-isolation result, not successful cancellation. The supervisor's continued
execution alone does not prove LED/network coexistence.

## Port and maintenance considerations

Pinned releases, not moving branches:

- [WAMR 2.4.5](https://github.com/wasm-micro-runtime/wasm-micro-runtime/releases/tag/WAMR-2.4.5),
  commit `25bd7eb63e828e4bd242cc9b38d260b4b31c6605`, released June 29, 2026.
  Its release addresses interpreter/loader security issues. Its upstream
  [ESP-IDF component](https://github.com/wasm-micro-runtime/wasm-micro-runtime/tree/WAMR-2.4.5/build-scripts/esp-idf)
  supports Xtensa and RISC-V, but the IDF 6 benchmark required local port wiring:
  exclude unused file/socket/clock/mapping implementations, route linear memory
  through the pool with zero initialization, and set a fixed native stack
  boundary with a 2 KiB cleanup guard. The interpreter source is unmodified.
- [wasm3 0.9.0](https://github.com/wasm3/wasm3/releases/tag/v0.9.0),
  commit `0cd38327f0c721e75172f4f1eeb55854dc0517af`, released August 24, 2026.
  This recent release adds module validation. The project README still describes
  [minimal maintenance](https://github.com/wasm3/wasm3#readme); do not treat that
  statement as abandonment or ignore its recent release. On GCC 15/Xtensa, the
  documented `M3_HAS_TAIL_CALL=0` fallback was necessary because the backend
  cannot emit the advertised `musttail` attribute. Its native stack cap and
  validation remained enabled. No source patch was applied.

Maintenance activity and these versions' features must be reassessed on upgrade;
all pins and port assumptions are tested inputs, not evergreen claims.

## Required production gates

Milestones 6.2–6.7 remain implementation work. In particular: runtime-neutral
ownership/API; bounded module loading and repeated lifecycle; per-call fuel plus
wall-clock cancellation; safe provider/UTF-8 pointer handling; registry-backed
script declarations; and a generated SDK/script LED layer. Gate D must inject
faults while actual LED output, serial/network control and settings remain live.
Gate C's outstanding physical timing/power qualification is not closed by this
standalone benchmark. No seven-board production WASM compatibility claim is made.

The three test boards were restored to the existing seven-board-qualified BLIP
fleet images and answered serial queries. NVS/storage and PC Wi-Fi were not
changed; the PC remained on `Archi-Wifi` with Internet connectivity.
