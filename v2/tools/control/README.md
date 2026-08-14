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
