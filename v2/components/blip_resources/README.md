# blip_resources

Fixed-capacity resource inventory, deterministic arbitration, move-only leases,
generation-tagged weak callback tokens, and board/profile conflict validation
under ADR-0002.

Work package 3.6 adds allocation revisions, complete inventory/claim snapshots,
and broker-backed atomic `swap` and `unassign-and-move` transactions. A stale
revision or failure at stop, persistence, or restart restores the old leases and
settings. Claim owners use stable `component:parameter` paths; shared bus members
remain distinct claims with member keys instead of collapsing into an "in use"
flag.

The broker is hardware-neutral. Target backends will translate granted logical
resources into ESP-IDF capability implementations; application components do
not include target driver headers.
