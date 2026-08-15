# Verification evidence

Evidence records are append-only and follow the requirements in
[`quality-gates.md`](../quality-gates.md). A record can pass only the gate row it
names; unmeasured rows remain open.

Raw serial logs sit beside their JSON record. Hardware descriptions distinguish
the board name supplied by the operator from chip and USB identities measured by
the tools.

Work-package-specific records are grouped by subsystem. A/B update interruption
and rollback records are under [`ota/`](ota/).

Disposable LED transport measurements and restoration checks are under
[`led-transport/`](led-transport/).

Gate B rollups are under [`gate-b/`](gate-b/). These records apply the exact
thresholds from `quality-gates.md`; completed work-package subflows are listed
as supporting evidence but do not turn an incomplete end-to-end row into a
pass.
