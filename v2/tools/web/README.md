# Device browser qualification

`blip_web_modes_hil.mjs` checks Simple/Advanced controls, saved naming, desktop and
phone layouts, graphs and optional dynamic script controls on the actual device.

`blip_web_installation_hil.mjs` checks automatic first-run installation from an
empty filesystem, refresh into the full app, plain-HTTP firmware hashing and a
real browser OTA upload followed by boot confirmation. It requires an existing
LAN connection and matching public local catalog. It never changes PC Wi-Fi.
Example (replace the device, port, MAC and local tool paths):

```powershell
node v2/tools/web/blip_web_installation_hil.mjs --device http://DEVICE-IP --bundle v2/components/blip_storage/factory_web.bundle --build build/m6-production-huzzah32 --preloaded-image build/preloaded/blip-v2.bin --flash-log build/empty-flash.log --mac DEVICE-MAC --port COM5 --python PATH-TO-IDF-PYTHON --playwright PATH-TO-PLAYWRIGHT/index.mjs --browser PATH-TO-BROWSER.exe --report build/first-run.json
```

Before flashing the empty filesystem, configure the local release endpoint over
serial. Preserve the application built with `BLIP_FLASH_WEB_UI=ON`, rebuild with
the option `OFF`, then full-flash using the generated arguments. The checker
asserts that both application files are identical. It must start before the
device's background first-run timer installs the UI; the initial web code must
be zero. Keep unrelated user files/settings in mind when preparing an empty
filesystem; the checker itself does not erase or flash storage.

For an already installed UI, pass `--existing-ui true` and omit
`--preloaded-image`. This additionally checks eleven malformed/incompatible image
prefixes before uploading the valid full image. Both modes leave the validated
firmware and interface installed. Reports capture binary, tool and source hashes.
The example invokes an actual firmware update and requires flashing authorization.

`blip_web_update_observer.mjs` observes the actual device UI while the native
recovery tool operates over serial. It verifies live identity, bounded automatic
WebSocket reconnection, one intentional asset-refresh navigation and final live
control. The parent tool starts/stops it and stores linked source/report hashes;
normal page loading during the refresh is allowed for at most five seconds.
