# ADR-0020: Lean private show-network defaults

- Date: 2026-10-09
- Status: Accepted by explicit user direction
- Supersedes: ADR-0019's mandatory HTTPS/authenticated-catalog policy and earlier
  mandatory production signing/access-control requirements
- Scope: All firmware components, web control, radio control and update tools

The user prioritizes firmware size, simplicity and reliable show operation on
private networks. Additional encryption, authentication, signed releases and
access-control layers are optional profile features. They are not required for
the standard firmware or a release gate. Do not introduce certificates, API
credentials, signing-key provisioning, encrypted application protocols or
authentication prompts into the normal device workflow.

Release checks and artifact downloads use plain HTTP by default. The default
URL is `http://www.goldengeek.org/blip/update`; it retains the requested host,
path and public GET metadata. The originally requested HTTPS URL remains usable
in an optional `BLIP_ENABLE_RELEASE_TLS=ON` build. TLS is omitted from the normal
HTTP client, along with its certificates and clock-sync requirement. Optional
HTTPS builds retain normal certificate verification and have higher flash/RAM
requirements; the user can choose those costs for a particular installation.

Device web control, OSC/OSCQuery, WebSocket and ESP-NOW/fleet remain usable without
additional application authentication or encryption. Existing Wi-Fi credentials
still connect to the selected network; this policy does not change that network's
configuration. Keep credentials write-only and out of logs. SDK secure-boot or
signed-image validation remains supported when explicitly enabled by an operator,
but no signing key or eFuse changes are required for ordinary production builds.

Keep memory bounds, parsing/type validation, resource ownership, device/profile
compatibility, sizes/checksums, atomic storage, A/B recovery and interruption
tests. These provide operational correctness even when authentication and
encryption are absent. SHA-256 remains a complete-file integrity check.

The release API is public JSON without passwords or API keys. A local plain-HTTP
simulator is the required development target. The user will host it online later;
remote deployment credentials and live publication are outside this task.

Earlier evidence remains historical evidence of the builds it names. Where old
plan or work-package text requires security features or live deployment, this
decision takes precedence. The user's current `quality-gates.md` edits are
preserved; this policy is recorded separately rather than rewriting that file.
