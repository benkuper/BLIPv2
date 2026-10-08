# ADR-0013: Component-owned WASM capability contract

- Status: Portable contract implemented; native import bridge qualification follows
- Date: 2026-10-08
- Related: [ADR-0001](0001-component-lifecycle-and-registry.md), [ADR-0010](0010-wasm-production-worker.md), [ADR-0012](0012-wasm-checked-utf8.md)

## Ownership and metadata

Each component optionally returns its own `CapabilityProvider` through the core
registry. Core needs only a forward declaration of this runtime-neutral interface;
it has no interpreter dependency. Function descriptors exist solely in the
component's immutable `ComponentDescriptor::wasm` field. Registry validation
rejects missing providers, undeclared providers and invalid function metadata
before starting hardware.

The import module is exactly `<component ID>.v<canonical decimal ABI version>`.
There is no implicit `env` alias or cross-component namespace. Breaking function
semantics or signatures require a new namespace version. Functions have named
numeric arguments (i32/i64/f32/f64), zero or one numeric result, descriptions and
a declared maximum callback duration of 1–2,000 microseconds. Paired i32 argument
roles describe UTF-8 offset/byte length for future generated bindings. They
describe the ABI; providers still validate all inputs before effects and use the
checked UTF-8 context for actual copies.

A component may declare eight functions with eight arguments each. Identifiers
are bounded ASCII public IDs; descriptions are at most 256 bytes; import module
names are at most 80 bytes. The bounded catalog accepts at most 64 components,
eight providers and 32 functions. It rejects shared provider instances, duplicate
namespaces and capacity excess without publishing partial bindings. Bind once,
then borrow immutable descriptor/provider pointers in lexical namespace/function
order. Registry descriptors, provider addresses and accessors remain stable
through runtime teardown. No allocation or parallel handwritten catalog occurs.

The existing descriptor JSON writer emits capability metadata from these same
descriptors. Components without capabilities retain their existing JSON shape.
Numeric values retain raw bit patterns, including unsigned values and NaNs.

## Invocation and lifecycle

Callbacks run exclusively on the script worker. They perform immediate bounded
queries or bounded queue admission; no I/O, allocation, render callbacks, guest
re-entry or unbounded waits. A trusted native callback cannot be forcibly
preempted by interpreter fuel. Its declared time bound must be measured and its
implementation reviewed before it is exposed. The portable catalog publishes
that bound; native bridge timing enforcement and measurement follow separately.

`CallContext` exposes cancellation and checked UTF-8 copies, never a native guest
pointer, engine handle or guest invocation method. It is borrowed only during
the callback and cannot be copied. Providers must not retain it or its spans.
The eventual bridge supplies only the currently executing module's memory;
service generation checks continue to protect external copies. The catalog
does not independently manage module lifecycle or generation.

Admission checks the exact signature, 32-bit widths, provider availability and
cancellation. Recursive dispatch is refused. Results pass through fixed scratch
and are published only after a successful callback with correct count/types and
no observed cancellation. Failure leaves caller output unchanged and count zero.
Completed provider effects are not rolled back by cancellation. UTF-8 writes
retain ADR-0012's per-copy failure atomicity, not transactional multi-call effects.

Provider availability and admission must synchronize with its owning component's
suspend/stop. Availability is a thread-safe advisory check, not a lifetime lease.
Composition must stop/join the script worker before provider destruction or
resource release. Native registration, rejection of undeclared imports, explicit
signature verification, attachment lifetime and start-failure cleanup will be
qualified in the next milestone 6.5 checkpoint. Until then WAMR continues to
reject every guest import.

## Evidence scope

Host tests establish metadata, independent component registration, limits,
failure atomicity, lifecycle admission, cancellation, recursion, raw numeric
bits and checked text behavior. Native qualification exercises the portable
catalog against real guest memory on a representative board. This is not yet
evidence of a guest-to-native trampoline, production provider exposure,
generated SDK bindings, script controls or full Gate D.
