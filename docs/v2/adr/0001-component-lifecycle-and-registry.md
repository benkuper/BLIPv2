# ADR-0001: Component lifecycle and registry

- Status: Accepted
- Date: 2026-08-14
- Owners: BLIP V2 core

## Context

V1 builds a compile-time component tree, registers path strings as setup occurs,
and exposes settings/OSCQuery through mutable parameter vectors. Initialization
is child-first, errors are booleans, dependencies are implicit singleton access,
and partial failure has no uniform cleanup contract. V2 requires one source of
truth for controls, settings, scripts, diagnostics, documentation, and UI.

## Decision

Every component has an immutable descriptor and a runtime instance. Descriptors
are registered before hardware is acquired. They contain:

- a stable namespaced component ID and schema version;
- typed parameters, actions, events, metadata, and diagnostic fields;
- explicit required/optional service dependencies;
- resource requests and feature/profile constraints;
- settings schema/current migration version;
- live-disable versus reboot-required policy;
- declared flash/RAM/task cost evidence.

The registry owns descriptors and instance handles and is the sole query surface
for OSCQuery, web schemas, persistence bindings, WASM capabilities, diagnostics,
and generated documentation. Those consumers may filter/transform registry data
but may not maintain parallel handwritten component catalogs.

Runtime states are:

```text
absent -> constructed -> validated -> starting -> running
                                      \-> failed
running <-> suspended
running|suspended|failed -> stopping -> stopped
```

Rules:

1. `construct` performs no I/O, task creation, registration callbacks, or
   resource acquisition.
2. Registry validation rejects duplicate IDs, invalid schemas, dependency
   cycles, missing required services, and impossible resource requests before
   any component starts.
3. Start order is a stable topological order. Equal nodes use component ID order
   so host and device runs are reproducible.
4. `start` receives only declared service/capability handles and granted resource
   leases. It must be safe to call once after successful validation.
5. On start failure, the failing instance is stopped first to clean up partial
   resources, then already-started components stop in reverse start order.
   The original error remains primary; cleanup errors are attached. If stop
   fails or callbacks remain active, retain that consumer and its transitive
   providers. Independent components can still stop. Retained instances remain
   in `stopping` so shutdown can be retried without restarting them.
6. `suspend` stops externally visible work without implying memory reclamation.
   It is idempotent. A descriptor states whether resume is supported.
7. `stop` is idempotent, cancels producers, drains or discards bounded work by
   declared policy, releases leases, and returns only when callbacks can no
   longer target the instance.
8. A stopped instance is not restarted in place unless its descriptor declares
   and tests that capability; normal reconfiguration constructs a new instance.
9. Registry mutation is closed before scheduling starts. Dynamic script-defined
   controls use a bounded registry extension transaction, not arbitrary vector
   mutation.

Component IDs and public field IDs are never reused with different meaning. A
rename creates an alias/migration entry and keeps the old ID reserved.

## Consequences

- Dependency and failure behavior can be tested on a host without ESP hardware.
- Startup has more up-front validation and descriptor data.
- Application components cannot discover unrelated global singletons.
- Generated UI/docs/protocol views remain consistent by construction.
- Dynamic features must fit a bounded extension model and may be rejected when
  capacity is exhausted.

## Required tests

Ordering, stable tie-breaks, duplicate IDs, missing dependencies, cycles,
validation without side effects, failure cleanup, cleanup error attachment,
idempotent suspend/resume/stop, callback quiescence, and bounded dynamic registry
extension are mandatory in work packages 1.2 and 1.3.

The 2026-10-08 [dependency retention evidence](../evidence/core/2026-10-08-provider-shutdown.json)
adds stop-error, active-callback and partial-start scenarios with transitive
providers, independent cleanup, retry and idempotence. This closes a registry
lifetime gap encountered while integrating component-owned WASM providers;
it does not establish the providers' hardware or timing qualification.
