# blip_led

The Milestone 3 vertical slice provides one persisted WS2812/SK6812 strip as
`blip.output.strip0`. The portable model defines GRB/GRBW order, brightness,
bounded settings, and 10 MHz RMT timings. The ESP-IDF adapter owns one native
RMT TX channel, a fixed 4 KiB pixel buffer, and a static output task.

Color/settings writes schedule the latest complete frame; the control callback
does not wait for the one-wire transfer. Pin and protocol changes require the
strip to be disabled, and enable/reconfiguration rolls platform state back if
persistence or RMT setup fails. The component uses ordinary RMT without DMA as
limited by [ADR-0007](../../../docs/v2/adr/0007-led-transport-qualification.md).

`BLIP_LED_DEFAULT_GPIO` supplies a board build's initial pin. It does not
override persisted settings.
