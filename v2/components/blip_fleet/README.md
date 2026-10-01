# blip_fleet

`fleet.hpp` implements the portable V2 fleet engine. It has no allocation,
radio calls, control dispatch, or blocking work. One adapter owns it and feeds
monotonic local microseconds into `service`, `receive`, and `next_message`.
Due commands are copied out with `take_due` for a separate control executor.

The lowest live 48-bit node ID leads a fleet. A random nonzero boot session
identifies each node incarnation. Heartbeats run every 250 ms; members expire
after two seconds. A leader transition cancels pending cues and establishes a
new clock epoch. Four retired sessions per live member reject reordered
heartbeats from recent boots. Discovery settles for 750 ms before a new leader
accepts a cue.

Followers use four timestamps: local probe send, leader receive, leader reply
send, and local reply receive. Samples with round trip above 10 ms are rejected.
Three good samples establish synchronization; accepted later offsets slew by
1/8 and reported time cannot go backward after the first sample in that epoch.
Losing samples for two seconds cancels pending cues. Software round trip and
offset estimates do not prove hardware output timing or a precision guarantee.

There are at most 16 members and eight pending cues. Commands are opaque byte
strings up to 512 bytes. Only the leader schedules them, with 100 ms to 60 s of
lead time. The leader broadcasts each command, retries every 100 ms until known
members acknowledge or the deadline arrives, and executes its own copy too.
Followers accept commands from their synchronized leader epoch only. A 32-cue
sequence window accepts reordering and ACKs duplicates without enqueueing
twice. Contradictory pending duplicates are rejected. Due commands are handed
off in deadline order; commands more than 20 ms late are dropped. An ACK means
queued, not executed.

The wire format is `BF`, version 2, kind, fleet ID, sender ID/session, leader
ID/session, sequence, payload size, three timestamps, deadline, CRC32, and
payload. The header is 76 bytes, integers are little endian, reserved bytes are
zero, and total size is at most 588 bytes. CRC32 detects corruption; it does
not authenticate a fleet peer. Network partitions elect independent leaders;
merging partitions selects the lowest live ID and cancels old epoch cues.

Host tests cover packet bounds/corruption, different device uptimes, packet and
ACK loss, duplicate and reordered cues, leader loss/reboot, stale sessions and
samples, capacity limits, and late cue rejection. Firmware transport and board
qualification are the remaining integration work for Plan 5.7.
