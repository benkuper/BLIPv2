# ADR-0017: Production script controls and bounded upload storage

- Date: 2026-10-08
- Status: Accepted for the production publication/action slice; full 6.6 remains open
- Extends: [ADR-0016](0016-wasm-owned-script-controls.md)
- Supersedes: the full fixed 96 KiB startup-buffer requirement in [ADR-0010](0010-wasm-production-worker.md), for the upload allocation only

The production WASM component owns the script-control store and one copied
message scratch outside its worker stack. Prepare declarations before engine
load, publish only after callback/global validation, and retire on replacement,
unload, engine fault and stop. Registry leases protect immutable retired
metadata; component retirement includes lease quiescence.

Script actions use the existing globally ordered request queue and completion
ring. Admission checks queue space, copies the store payload and sends its
marker under the same admission mutex. The worker dequeues even canceled
payloads, checks their ticket/epoch/module/schema generation, copies checked
strings into the guest arena and invokes the callback under captured budgets.
Separate immediate action-queue admission prevents unrelated parameter/event
traffic from starving payload retirement. Upload begin/commit and unload close
script-action admission when accepted. A commit reopens it only when no later
replacement is pending. Publication after stop/cancellation fails closed.

The engine/linear-memory reservation remains fixed at 80 KiB. The pool is
reserved by the lifecycle owner after registry validation and before
network services fragment internal RAM. Interpreter/task startup still follows
registry dependency order. The S3 profile could not satisfy the aligned pool
allocation after Wi-Fi startup; early reservation restores normal boot without
changing the pool bound or exposing engine state before dependency startup.
The mutable
upload allocation is worker-owned, bounded by 16 KiB, and sized to the accepted
upload. Unload the engine and detach its borrowed scratch before freeing or
resizing that allocation. Failed allocation leaves no runnable module and
returns an explicit bounded failure; a later upload can retry. The service
checks both configured maximum and actual borrowed capacity before copying.
Routine stops retain reservations; permanent release requires worker/lease
quiescence. The cost descriptor reports the maximum, while `buffer_reserved`
reports actual current allocation.

Keeping a full 16 KiB upload buffer alongside the new control store exhausted
the combined Ball BLE/WASM HTTP workload: an early functional trial stalled
with roughly 4 KiB free heap and a 1.5 KiB largest block. Upload-sized storage
restored full schema responses and stable warmed heap for small modules while
preserving the 16 KiB input limit. That limit is an admission bound, not a
guarantee that every module fits available heap or the engine pool. Allocation
occurs only during upload lifecycle on the worker, outside render/output paths.

This slice publishes parameter/action/event declarations and executes copied
typed actions. Guest parameter/event imports, copied external event delivery,
transport schema tokens and full-width scalar support, automatic web schema
refresh and the SDK remain separate required work. No full Gate C/D or complete
work-package 6.6 result is claimed.
