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

Provision from a local two-line credentials file (SSID, then password) without
putting the password in shell history or the process argument list:

```powershell
python v2/tools/control/blip_wifi_provision.py --port COM8 --credentials wifi.txt
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

Network HIL must use the atomic wrapper below on a one-Wi-Fi-interface host.
The wrapper performs OSCQuery discovery, browser WebSocket and UDP writes while
offline, then restores the named Internet profile in an unconditional `finally`
block before returning:

```powershell
& v2/tools/control/blip_ap_control_hil.ps1 -DeviceSsid BLIP-7FAC18 -InternetProfile Archi-Wifi
```
