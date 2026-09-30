# BLIP serial control utility

The utility sends the COBS-framed BLIP envelope v1 over UART or native USB
Serial/JTAG. It needs Python 3.10 or newer and pyserial for hardware access.

```powershell
python -m pip install pyserial
python v2/tools/control/blip_serial_control.py --port COM8 get blip.bootstrap probe_value
python v2/tools/control/blip_serial_control.py --port COM8 set blip.bootstrap probe_value 27
python v2/tools/control/blip_serial_control.py --port COM8 action blip.bootstrap reset_probe
```

Values infer `true`/`false`, integer, number, then string. Prefix a value with
`b:`, `i:`, `n:`, or `s:` to force its type. Device and protocol failures are
printed as JSON on standard error and return exit code 2.

Provision from the Git-ignored `v2/firmware/local-wifi.txt` (SSID on the first
line, password on the second) without putting the password in shell history or
the process argument list:

```powershell
python v2/tools/control/blip_wifi_provision.py --port COM8 --credentials v2/firmware/local-wifi.txt
```

Keep that credentials file excluded from version control. The helper prints
only the SSID and success state.

The codec-only check has no external dependency:

```powershell
python v2/tools/control/blip_serial_control.py --self-test
```

The LED hardware-in-loop helper configures the strip, checks typed rejection of
a live transport change, and then verifies persisted settings after an external
reset. Run its two phases around a board reset:

```powershell
python v2/tools/control/blip_led_serial_hil.py --port COM8 --phase configure --pin 20 --expect-antenna 1
# reset the board
python v2/tools/control/blip_led_serial_hil.py --port COM8 --phase verify --pin 20 --expect-antenna 1
```

Prefer shared Wi-Fi for network HIL. If a device AP is required on a host with
one Wi-Fi interface, use the wrapper below. It starts a separate timed recovery
process before switching Wi-Fi. The process reconnects the Internet profile if
the test process dies or exceeds its deadline. The wrapper restores and verifies
Internet access on the normal path; `-RecoveryDelaySeconds` defaults to 600:

```powershell
& v2/tools/control/blip_ap_control_hil.ps1 -DeviceSsid BLIP-7FAC18 -InternetProfile 'Archi-wifi guest'
```

To check Art-Net discovery and send a short DMX test on shared Wi-Fi:

```powershell
python v2/tools/control/blip_artnet_probe.py --host 192.168.27.186 --pixels 36 --red 32 --frames 30
```

For directed broadcast discovery, pass the subnet's broadcast address as
`--host`, add `--broadcast`, and use `--expect-ip` to identify the intended
node. The probe checks the 239-byte PollReply and then unicasts DMX frames to
the responding node.

The E1.31 sender can check unicast and universe multicast reception without
changing the PC's Wi-Fi connection:

```powershell
python v2/tools/control/blip_e131_send.py --host 192.168.27.186 --pixels 36 --red 32 --frames 30
python v2/tools/control/blip_e131_send.py --host 192.168.27.186 --multicast --pixels 36 --red 8 --frames 10 --terminate
```

This receiver currently uses universe 1, accepts at most two sources, and
ignores preview and synchronized data packets. The serial parameters on
`blip.input.e131` report accepted, rejected, ignored, and multicast status.

For a bounded simultaneous Art-Net, DDP, and E1.31 load on a board already
connected to shared Wi-Fi, run:

```powershell
python v2/tools/control/blip_led_network_soak.py --host 192.168.27.186 --port COM10 --pixels 36 --duration 180 --sample-seconds 30 --out docs/v2/evidence/board-bringup/ball-network-soak.json
```

New reports include sent packet counts and serial snapshots of accepted/rejected
packets, applied/failed LED frames, stack headroom, and heap. `elapsed_seconds`
includes the final serial sample; `traffic_duration_seconds` measures the send
period. This is a short load check, not waveform timing qualification.

To check the calculated LED current limit and restore the prior strip settings,
run the serial HIL helper. Use a budget at least as large as the configured
pixel count because the model includes 1 mA idle per pixel:

```powershell
python v2/tools/control/blip_led_power_hil.py --port COM10 --budget-ma 50 --red 255 --out docs/v2/evidence/board-bringup/ball-power-limit.json
python v2/tools/control/blip_led_power_hil.py --port COM5 --budget-ma 15 --red 255 --out docs/v2/evidence/board-bringup/huzzah-power-limit.json
```

Add `--temporarily-enable` if the strip is disabled. The helper restores the
prior red channel, enabled state, and budget in a `finally` block and reports
whether restoration was verified. Its result is a calculated-current check;
physical supply current still needs a meter.
