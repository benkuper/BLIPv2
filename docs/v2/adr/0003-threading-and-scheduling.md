# ADR-0003: Threading, scheduling, and event delivery

- Status: Accepted
- Date: 2026-08-14
- Owners: BLIP V2 core

## Context

V1 has a normal loop and one high-priority polling task. Components can call one
another synchronously, perform variable work in update callbacks, and forward
events immediately. LED, storage, logging, and networking boundaries are not
enforced.

## Decision

V2 uses a small fixed set of task classes. The initial classes are:

| Class | Purpose | May block? |
| --- | --- | --- |
| render/output | compositor, buffer handoff, driver completion | no |
| control | registry actions, parameters, lifecycle, bounded event dispatch | only on bounded local synchronization explicitly declared |
| transport | serial/radio/network parsing and transmit queues | on driver/network waits with timeouts |
| storage/maintenance | files, settings commits, OTA, low-priority metrics | yes, with cancellation and timeouts |

Components schedule bounded jobs onto a class; they do not create arbitrary
tasks. Exceptions require an ADR plus declared stack/priority/core-affinity cost.
The scheduler uses monotonic time and fixed-capacity queues. Every job declares a
maximum execution budget, queue overflow policy, cancellation behavior, and
diagnostic name.

Communication between components uses typed service calls for immediate bounded
queries, bounded queues for work transfer, and typed events for fan-out. No
component holds or reaches through an unrelated global singleton.

Event rules:

1. Payload ownership and lifetime are explicit; queued events never point to a
   producer stack frame.
2. Queue capacity is fixed at startup. Overflow policy is one of reject-new,
   drop-oldest, coalesce-by-key, or fault; it is part of the descriptor.
3. Overflow increments a metric and produces a rate-limited diagnostic outside
   real-time context.
4. Event subscribers execute in declared task context and cannot recursively
   dispatch an unbounded event chain.
5. Stop cancels producers and invalidates generation tokens before queues are
   drained/discarded.

The render/output class has the strictest boundary: no heap allocation,
filesystem access, logging, networking, or component callback whose worst-case
time is not statically bounded. It receives immutable frame inputs or owned pool
buffers prepared elsewhere. Instrumented test builds turn forbidden operations
into test failures.

## Consequences

- Backpressure and overload outcomes are deterministic and observable.
- Some synchronous V1 behavior becomes asynchronous and needs completion events.
- Fixed queues consume known static/pool memory even when idle.
- Timing evidence can be tied to named task classes and budgets.

## Required tests

Queue overflow for every policy, execution budget overrun, cancellation, stop
with queued work, recursive event protection, monotonic scheduling across timer
wrap simulation, task-context assertions, and all Gate C forbidden-operation
checks are required.

