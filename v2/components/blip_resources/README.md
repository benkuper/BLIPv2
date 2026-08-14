# blip_resources

Fixed-capacity resource inventory, deterministic arbitration, move-only leases,
generation-tagged weak callback tokens, and board/profile conflict validation
under ADR-0002.

The broker is hardware-neutral. Target backends will translate granted logical
resources into ESP-IDF capability implementations; application components do
not include target driver headers.
