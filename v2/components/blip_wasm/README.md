# blip_wasm

Reserved for the runtime-neutral WASM service, execution limits, and
component-owned capability providers.

[ADR-0008](../../../docs/v2/adr/0008-wasm-runtime-selection.md) selects the metered
WAMR fast interpreter after the three-family hardware comparison. The
[benchmark](../../qualification/wasm/README.md) is a separate qualification
project; its adapter is not the production service. Milestones 6.2–6.7 and Gate D
remain open.
