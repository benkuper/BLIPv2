# ADR-0002: Resource ownership and hardware capabilities

- Status: Accepted
- Date: 2026-08-14
- Owners: BLIP V2 resources

## Context

V1 components configure pins, buses, timers, PWM channels, filesystems, and
radios directly. Conflicts depend on build macros and initialization order. This
prevents reliable profile generation and makes live enable/disable unsafe.

## Decision

A resource broker is the only allocator of shared or exclusive hardware
resources. Board manifests declare what exists and which resources are reserved.
Feature manifests declare every request. Components receive move-only leases and
capability interfaces; they do not call target-specific ESP-IDF drivers directly.

Resource classes include GPIO, ADC, touch, RMT, LEDC/MCPWM, SPI host/device and
chip-select, I2C controller/address, UART, timers, DMA channels/descriptors,
internal/PSRAM memory pools, filesystem mounts, USB endpoints, Wi-Fi/BLE/ESP-NOW
radio modes, and power-management locks.

Each request states:

- resource class and stable logical name;
- exclusive, shared-read, bus-member, or multiplexed ownership;
- required capabilities and acceptable alternatives;
- pins/electrical mode/frequency/timing/DMA/memory bounds as applicable;
- acquisition phase and whether live release/reacquire is supported;
- incompatible features and radio coexistence constraints.

Static manifest conflicts fail generation/build. Conflicts dependent on runtime
settings fail registry validation before component start. The broker has a
deterministic allocation order and returns typed error details naming both
claimants and the unsatisfied constraint.

A lease is released explicitly during component stop and defensively by its
destructor. Driver callbacks hold a generation-tagged weak token so a late ISR or
completion cannot call a stopped component. ISR installation/removal and bus
teardown are owned by the capability implementation, not application components.

Board code implements capabilities such as `DigitalInput`, `PixelOutput`,
`Storage`, and `RadioSession`. Components depend on those interfaces. A target
backend may use ESP-IDF GPIO/RMT/SPI/etc.; the component may not include those
driver headers.

## Consequences

- Pin and peripheral conflicts become understandable configuration errors.
- Live feature changes can distinguish suspension from actual resource/memory
  reclamation.
- Some formerly simple driver calls require capability wrappers and manifests.
- Cross-target host fakes and hardware-specific implementations share the same
  component contract.

## Required tests

Conflicting/exclusive leases, legal shared buses, alternative selection,
deterministic allocation, rollback after partial grant, late callback rejection,
release/reacquire, reserved board resources, radio coexistence, and build-time
manifest diagnostics are mandatory for work package 1.4.

