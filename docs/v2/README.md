# BLIP V2 architecture and evidence

This directory records the contracts that must be stable before production V2
firmware starts. The baseline is BLIP V1 commit
[`e567eeb5f20ba022595fd5b89a77fb17ba59ca94`](https://github.com/Golden-Geek/BLIP/commit/e567eeb5f20ba022595fd5b89a77fb17ba59ca94).
The repository does not vendor that source; every V1 observation below names the
pinned commit and source path used to derive it.

## Deliverables

| Deliverable | Location |
| --- | --- |
| Baseline and evidence policy | [source-baseline.md](source-baseline.md) |
| Complete V1 component and behavior inventory | [v1-inventory.md](v1-inventory.md) |
| Public compatibility matrix | [compatibility.md](compatibility.md) |
| Target profiles and measurable gates | [quality-gates.md](quality-gates.md) |
| PR and issue dependency map | [dependency-map.md](dependency-map.md) |
| Architecture decisions | [adr/](adr/) |
| V1 compatibility fixtures | [`v2/tests/fixtures/v1/`](../../v2/tests/fixtures/v1/) |
| Implementation work-package records | [work-packages/](work-packages/) |
| Milestone 1 verification | [work-packages/milestone-1-evidence.md](work-packages/milestone-1-evidence.md) |
| Work package 2.1 settings contract | [work-packages/2.1-versioned-nvs-settings.md](work-packages/2.1-versioned-nvs-settings.md) |
| Work package 2.7 OSC/OSCQuery contract | [work-packages/2.7-osc-oscquery.md](work-packages/2.7-osc-oscquery.md) |
| Work package 3.1 schema-driven web shell | [work-packages/3.1-schema-web-shell.md](work-packages/3.1-schema-web-shell.md) |
| Append-only gate evidence | [evidence/](evidence/) |

## Scope boundary

Milestone 0 contains documentation, manifests, fixtures, and host-side fixture
validation only. It intentionally contains no ESP-IDF application or production
component implementation. V1 public behavior is documented, not silently
redesigned. The V2 contracts use versioned envelopes and schemas from their
first implementation even where V1 did not.

Milestone 1 adds the independently buildable V2 core beside that preserved
baseline. Its work-package records distinguish host/build evidence from the
physical-board evidence still required to close Gate A.

Work package 2.1 is implemented in the working V2 line but remains merge-gated
by Gate A. Its record fixes the first V2 persisted byte format and distinguishes
host interruption simulation from hardware qualification.

## Decision status

All ADRs in this milestone are **Accepted**. A later change requires a new ADR
that supersedes the old one; editing an accepted decision in place is reserved
for spelling and clarification that do not change its contract.

## Definition of done

Milestone 0 is complete when:

1. Every compiled or placeholder V1 component is represented in the inventory.
2. Each known public control, discovery, settings, file, and radio surface has a
   compatibility disposition.
3. The checked-in fixture manifest validates without network access or hardware.
4. Target/profile manifests validate against their schema.
5. Every later work package has an explicit dependency and acceptance evidence.
6. No V1 source file has been changed, moved, or deleted.
