# blip_fleet

Routerless fleet coordination uses ESP-NOW broadcast. A node can join by using
its fleet ID and fixed radio channel; no router, IP address, computer, phone,
paired-device list, or complete participant roster is required. Followers are
silent during steady operation. Leader beacons and repeated cues consume the
same airtime regardless of listener count. Actual capacity depends on range,
interference, startup contention and command rate; this is a single-hop design.

## Election and clock

The lowest node ID whose beacon is heard leads. At startup and after two seconds
without its leader, a node waits a jittered 20?500 ms before claiming leadership.
Lower IDs challenge a higher leader, and higher IDs yield after hearing a lower
one. Discovery settles for 750 ms before a new leader accepts scheduled work.
Leader changes cancel pending commands and create a new clock epoch. Partitions
can form independent fleets; merging selects the lower heard leader.

Only leaders send clock beacons, every 250 ms. Each listener estimates its clock
offset from the leader's transmit timestamp and its own receive-callback timestamp.
An eight-sample window selects the least-delayed estimate; later changes slew by
1/8. Three samples lock the clock. Large negative outliers are rejected. Losing
samples for two seconds unlocks the clock and cancels queued commands. Time stays
monotonic within an epoch. This one-way estimate cannot measure network delay:
`uncertainty_us` reports an assumed 5 ms transport budget, not a proven bound.
Physical timing qualification remains necessary.

## Cues and delivery

A leader accepts eight pending commands, each at most 488 bytes, with 100 ms to
60 s lead time. It broadcasts each command repeatedly at 100 ms intervals until
its deadline and executes a local copy. Listeners keep a 32-cue sequence window
for four recent leader incarnations, accepting reordering and rejecting conflicting
duplicates. Known cancelled cues are not requeued when a former leader returns
within this retained history. No global delivery ACK or participant census is
claimed. Packet loss can prevent a listener from receiving a cue before its
execution time; repetition improves delivery but does not guarantee it.

The portable engine allocates no memory and performs no radio or control calls.
Its memory is fixed below 6 KiB per device, independent of listener count. A
separate control executor receives due commands in deadline order. Commands over
20 ms late are dropped. Epoch fences and another lateness check protect queued
executor work after reconfiguration, clock loss or election. Already executing
commands finish; cancellation does not undo their effects. Slow commands may
cause later cues to be dropped, while clock traffic and direct control continue.
Shared settings transactions hold a mutex around their common scratch buffer.

## ESP-IDF integration

Build with `BLIP_ENABLE_FLEET=ON`; the default build excludes it. The component
`blip.fleet` defaults to disabled, fleet ID 1 and channel 1. Fleet ID and channel
must match on every prop. Channel settings currently permit 1?11. Fleet settings
schema 2 migrates the earlier V2 prototype record by choosing channel 1.

Active mode takes an autonomous radio lease from the Wi-Fi owner. The device
stops association/scanning, runs ESP-NOW station radio on the fixed channel, and
keeps its setup AP. Saved network credentials and settings are retained. Disabling
fleet releases the lease and restores saved networking. A latency lease suspends
modem sleep while fleet is active, restoring the previous policy on release.
The Wi-Fi controls expose `autonomous_channel`, `low_latency_clients` and
`latency_policy_failures`. Wi-Fi RF must be enabled and its driver loaded; a boot
profile that reclaims that driver cannot run ESP-NOW. The PC's Wi-Fi is never
changed by the HIL helper.

A shared ESP-NOW owner multiplexes the SDK's single receive callback between
broadcast fleet and unicast peer control. The broadcast address consumes one
hardware peer entry, irrespective of listener count. Unicast control normally
resumes after saved station networking is restored. Broadcast reception only
queues bounded packet copies from the Wi-Fi task. Beacon timestamps are captured
there rather than when the worker later processes them.

The wire header is 48 bytes: `BF`, version 2, kind, fleet ID, sender ID/session,
sequence, send timestamp, cue deadline, payload size, reserved zeros and CRC32.
Integers are little endian. Packets fit the existing 536-byte V2 ESP-NOW
fragmentation buffer and take at most three 250-byte radio packets. Each repeated
transmission has a new fragment message ID; application cue IDs remain stable for
duplicate suppression. CRC and fleet ID do not authenticate commands. This is
currently an opt-in trusted-fleet prototype; authenticated broadcast is future
work.

The leader exposes typed `schedule_write_integer`, `schedule_write_boolean`,
`schedule_write_number`, and `schedule_write_string` actions taking component,
parameter, value and delay in milliseconds. `schedule_action` takes component,
action and delay for an action without arguments. They return a queued cue ID;
target validation happens when each node executes. The C++ `schedule` entry point
accepts a full `ControlRequest`, including action arguments. Scheduling fleet
reconfiguration itself is rejected. `cancel_local` cancels this node's pending
work. No total member count is exposed because listeners are not enumerated.

## Validation

Host cases cover election, different uptimes, loss and retries, duplicate and
reordered cues, leader reboot/loss, partition merging, stale samples, bounds and
lateness. A 4,096-listener simulation checks silent followers and constant
broadcast work; it does not prove 4,096-device radio performance.

Run `python v2/tools/control/blip_fleet_hil.py --ports COM10 COM5 --out report.json`
for the routerless pair gate. It checks mode, election, clock lock, scheduled writes
and actions, direct serial control, leader withdrawal and rejoin, then verifies
restoration of fleet/probe settings and both radio leases.

The transport choice follows [Espressif's ESP-NOW FAQ](https://docs.espressif.com/projects/esp-faq/en/latest/application-solution/esp-now.html):
broadcast control is not constrained by the 20 paired-device protocol limit.
