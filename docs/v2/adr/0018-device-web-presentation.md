# ADR-0018: Simple/Advanced device web presentation

- Date: 2026-10-08
- Status: Accepted for the graphical interface foundation
- Extends: registry/OSCQuery schema, dynamic control lease contracts

The device serves one dependency-free app with Simple as its default mode.
Advanced renders every published parameter/action/event and resource assignment,
with topic navigation and search. Mode changes use the same live control model.
No component-specific browser panel or fixed device OSC path is required.

Component-owned metadata supplies `ui_topic` (human topic), `ui_primary` (comma
separated projected OSCQuery control IDs for Simple), and `ui_gauges` (IDs with
declared numeric ranges suitable for a meter). OSCQuery exposes those hints in
`BLIP_UI` as `TOPIC`, `PRIMARY`, `GAUGES`. IDs in these lists match projected
leaves, including any legacy aliases. Missing hints retain generic Advanced
controls; namespace-based topic fallback keeps old/new components inspectable.
Leased script controls have `BLIP_DYNAMIC:true` and appear in Simple as well.
Labels and metadata are text, never executable HTML.

Simple uses switches, bounded sliders, numeric readouts, recent-value graphs and
declared-range meters. Graphs use real readings only and retain at most 30
samples per visible numeric control; they are qualitative trends, not calibrated
measurement plots. Readings retain their descriptor labels/units. Write-only
values stay hidden. Backend access/type/range/resource checks remain authoritative.

The browser polls the full schema every three seconds while visible, with
bounded backoff on failures. A shape/generation change replaces editors; value
changes update existing controls and preserve focused drafts. WebSocket replies
also update the same model. Schema refresh does not yet bind external requests
to a schema generation: transport tokens and lossless i64 remain required in
work package 6.6. Declared diagnostics outside parameter surfaces also require
their complete inspection path before claiming the wider 3.7 acceptance.

The app loads ES modules sequentially in dependency order. Browser parallel
imports exceeded the four HTTP-session budget and evicted active responses in
the production profile; raising the RAM budget is not required for this app.
Native fetch retains its Window receiver. Normal browser Accept headers fit a
bounded 256-byte reader. When an app delegate runs, the home page serves the app
on both station/AP; `/setup` retains the independent Wi-Fi recovery form. Without
a running app delegate, the provisioning fallback remains available.

Web bundle 0.2.0 is independently versioned. Website release checks/downloads,
the update center, optional automatic policy and remote release qualification
are separate required work packages 3.8/3.9. This ADR does not claim those systems
or complete Gate B/C/D qualification.
