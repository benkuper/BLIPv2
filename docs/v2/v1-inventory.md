# BLIP V1 component and behavior inventory

This is the source-derived inventory for V1 commit
`e567eeb5f20ba022595fd5b89a77fb17ba59ca94`. It separates
behavior that is actually registered and reachable from code that merely exists
behind a build flag. Names and capitalization are preserved because they are
part of the public path contract.

## Runtime model

`RootComponent` owns a compile-time-selected tree. During `setup`, each component
registers a script-style path such as `leds.strip1.playbackLayer`; this flat path
is used to route incoming commands. A component normally exposes:

- `enabled` as a persisted, writable boolean;
- optional `updateRate` and `feedbackRate` when a component opts in;
- parameters tagged `Config` (persisted and writable), `Feedback` (read-only),
  or untagged (writable but not persisted);
- zero-argument triggers and custom commands;
- named events forwarded by enabled transports.

The same tree is serialized as OSCQuery. Slash paths are used for OSC and
OSCQuery; dot paths are used for serial input and the internal router. V1 has no
dependency graph, resource broker, schema version, or transactional lifecycle.

Lifecycle order is `setup` parent-first, `init` child-first, periodic `update`,
and `clear` parent-internal-first then children. A separate FreeRTOS task calls
components marked high priority every tick. Components marked critical continue
while non-critical updates are globally suspended.

## Active V1 build environments

| Environment | MCU/board | Main feature set | Notes |
| --- | --- | --- | --- |
| `creatorsball` | ESP32-C6 DevKitM-1 | network, LED, Art-Net, FX, button, battery, script, ESP-NOW, IR, BNO055, OTA | WS2816, 37 pixels, external SPI storage |
| `creatorsballv2` | ESP32-C6 DevKitC-1, 8 MiB | same, BNO08x | default environment; HD108, 36 pixels |
| `creatorsclub` | ESP32 DevKit | same, BNO055 | SK9822, 32 logical pixels (three physical LEDs each), SD storage; V2 `creators-club` profile |
| `poebridge` | Olimex ESP32-POE-ISO | Ethernet, Art-Net, button, ESP-NOW bridge, OTA | LittleFS fallback |
| `disneybridgeA0` | ESP32-C6 DevKitM-1 | Art-Net, button, GPIO, OTA, RF24/Flowtoys, DIP, wired DMX | bridge-specific pins |

The repository contains commented M5Stick and XIAO C6 environments. They are
historical configuration evidence, not active build acceptance targets.

## Component inventory

Legend: **cfg** is persisted and writable, **rw** is writable but not persisted,
and **ro** is feedback/read-only. Every normal component also has `enabled`
(**cfg**) unless stated otherwise.

| Build flag / tree path | Component and behavior | Public parameters | Actions / events | Hardware and dependencies |
| --- | --- | --- | --- | --- |
| Always, root `/` | `RootComponent`: owns the tree, routes commands, starts the fast task, and coordinates restart/deep sleep | no root `enabled` exposure | triggers `shutdown`, `restart`, `standby`, and when both radios exist `switchToWifi`, `switchToESPNow`; custom `stats`, `log` | FreeRTOS task, reset and sleep APIs |
| Always, `/comm` | `CommunicationComponent`: transport fan-in/fan-out | only inherited fields | event `MessageReceived` | owns enabled transports |
| `USE_SERIAL`, `/comm/serial` | newline serial command/feedback transport at 115200 baud | `sendFeedback` (**cfg**) | `yo` discovery; event `MessageReceived` | UART/USB CDC `Serial`; fixed 512-byte input buffer |
| `USE_OSC`, `/comm/osc` | UDP OSC discovery, command, and feedback | `remoteHost`, `remotePort`, `sendFeedback`, `isAlive` (**cfg**, including the state-like `isAlive`) | `/yo`, `/ping`; event `MessageReceived` | UDP 9000, default remote port 10000, mDNS `_osc._udp` and `_oscjson._tcp` |
| `USE_SERVER`, `/comm/server` | HTTP OSCQuery, static files/upload, and OSC-over-WebSocket | `sendFeedback`, `sendDebugLogs`, `suspendUpdatesDuringUpload`, `suppressFeedbackDuringUpload` (**cfg**) | events `UploadStart`, `Uploading`, `UploadDone`, `UploadCanceled` | TCP 80; WebSocket `/`; filesystem for assets/uploads |
| `USE_ESPNOW`, `/comm/espnow` | ESP-NOW control, pairing, and stream transport | common: `pairingMode`, `longRange`, `optimalRange`, `channel` (**cfg**); node: `autoPairing`, `pairOnAnyData`, `sendFeedback` (**cfg**), `wakeUpMode` (**rw**); bridge: broadcast/range/stream/test/routing fields (**cfg**) and `wakeUpMode` (**rw**) | event `MessageReceived`; bridge custom `clear` | 2.4 GHz radio; bridge stores up to 10 peer MAC strings; 250-byte payload buffer |
| `USE_WIFI`, `/wifi` | Wi-Fi station/AP/AP+station and optional Ethernet connection manager | `mode`, `ssid`, `pass`, `manualIP`, `manualGateway`, `channelScanMode`, `txPower`, `wifiProtocol` (**cfg**); `signal` (**rw**) | custom `info`; event `ConnectionStateChanged` | Wi-Fi; optional Ethernet; shares radio/channel with ESP-NOW |
| Always, `/settings` | identity and NVS save/clear interface | `propID`, `deviceName` (**cfg**); `deviceType`, `firmwareVersion` (**ro**); optional `wakeUpButton`, `wakeUpState` (**cfg**) | triggers `saveSettings`, `clearSettings`, `factoryReset`; aliases `save`, `show`, `clear` | NVS namespace `blip`, key `settings` |
| `USE_FILES`, `/files` | mounts SD, SD_MMC, external SPI flash, or LittleFS fallback; file management | SPI pins and optional SD enable/speed (**cfg**) | `delete`, `deleteFolder`, `list`, `format`, `info`, `deleteAll`; events `uploadStart`, `uploadProgress`, `uploadComplete`, `uploadCancel`, `list`, `info` | SPI/SD/MMC/LittleFS; `deleteAll` logs only in the pinned code |
| `USE_BATTERY`, `/battery` | filtered ADC/fuel-gauge battery state and shutdown trigger | ADC/charge pins, calibration, low threshold, charge LED intensity, shutdown timeouts (**cfg**); `batteryLevel`, `voltage`, `charging` (**ro**) | event `CriticalBattery` | ADC, GPIO, optional MAX17048/I2C; 30-sample window |
| `USE_LEDSTRIP`, `/leds` | fixed/dynamic strip manager | `count` (**cfg**) unless fixed | none | owns `stripN` |
| `USE_LEDSTRIP`, `/leds/stripN` | LED output and four-layer compositor | `count`, `dataPin`, `enPin`, `clkPin`, `invertStrip`, `multiLedMode`, `maxPower` (**cfg**); `brightness`, `colorCorrection` (**rw**) | none | FastLED or NeoPixelBus; static `LED_MAX_COUNT` buffers; target-specific driver macros |
| implicit, `/leds/stripN/playbackLayer` | raw frame playback | `blendMode`, `idMode`, `loop` (**rw**) | `load`, `play`, `playSync`, `pause`, `resume`, `stop`, `seek`, `unload`; events `Loaded`, `LoadError`, `Playing`, `Paused`, `Stopped`, `Seek`, `Looped` | `/playback/*.meta` plus `*.colors`; filesystem; optional scripts |
| Art-Net/DMX, `/leds/stripN/streamLayer` | maps DMX channels into pixels | `blendMode` (**rw**); `universe`, `startChannel`, `use16Bits`, `includeAlpha`, `clearOnNoReception`, `noReceptionTime` (**cfg**) | none | `DMXReceiverComponent` listener |
| `USE_SCRIPT`, `/leds/stripN/scriptLayer` | generic script-owned pixel layer | `blendMode` (**rw**) | script helper API exists in comments, not linked in pinned code | shared static color buffer |
| Always with strip, `/leds/stripN/systemLayer` | connection, battery, pairing, shutdown, and critical-state overlay | `blendMode`, `showBattery`, `espSyncColor` (**rw**) | none | reads Wi-Fi, battery, ESP-NOW, and root state directly |
| `USE_FX`, `/leds/stripN/fx` | motion-reactive offset/isolation post-process | `staticOffset`, `offsetSpeed`, `isolationSpeed`, `isolationSmoothing`, `isolationAxis`, `swapOnFlip`, `showCalibration` (**cfg**) | none | directly reads motion state |
| `USE_PWMLED`, `/pwmLed/pwmLedN` | RGB/RGBW PWM LED output, optional DMX streaming | pins, resolution, frequency, alpha/RGBW and stream mapping (**cfg**); `color` (**rw**) | none | LEDC PWM; optional DMX listener |
| `USE_IO`, `/gpio/gpioN` | generic digital, analog, oscillator, or touch GPIO | `pin`, `mode`, `inverted` (**cfg**); `value` (**rw**) | value feedback on change | GPIO, ADC, touch, LEDC depending on mode |
| `USE_BUTTON`, `/buttons/buttonN` | GPIO input plus short/multi/long/very-long press state | inherited pin/mode/inverted (**cfg**), `canShutDown` (**cfg**); `value` (**rw**), `multiPressCount`, `longPress`, `veryLongPress` (**ro**) | root interprets long press, demo count, and ESP-NOW pairing counts | GPIO; 500 ms long, 1500 ms very long, 300 ms multi-press window |
| `USE_IR`, `/ir` | combines PWM measurements from one or two IR receiver pins | `pin1`, `pin2`, `value`, `keepValueOnReboot` (**cfg**) | value feedback | GPIO/LEDC input measurement |
| `USE_MOTION`, `/motion` | BNO055, BNO08x, or M5 IMU acquisition and derived throw/activity/spin values | connection/send policy, I2C pins, thresholds/offsets (**cfg**); orientation, accel, gyro, linearAccel, projectedAngle, spinCount, spin, activity (**ro**); `throwState` (**rw**) | `calibrationStatus`; events `orientation`, `accel`, `gyro`, `linearAccel`, `throwState`, `calibration`, `activity`, `debug`, `projectedAngle` | I2C and optional IRQ; component forces itself disabled on setup but does not persist `enabled` |
| `USE_DISTANCE`, `/distances/distanceN` | HC-SR04 or VL53L0X normalized distance | trigger/echo or connection state, max distance, debounce frames, send rate (**cfg/rw as tagged**); `value` (**ro**) | none | GPIO timing or I2C sensor |
| `USE_DIPSWITCH`, `/dipswitch` | nine-bit DIP value | `value` (**ro**) | none | TCA6408 I2C expander plus one GPIO; pin macros are compile-time only |
| `USE_DMX` or `USE_ARTNET`, `/dmxReceiver` | receives wired DMX, Art-Net, or ESP-NOW stream and dispatches to listeners | only inherited fields; `updateRate` is configurable | none | UART-style DMX pins and/or UDP Art-Net; optional activity LED |
| `USE_SCRIPT`, `/script` | wasm3 script loader, tick loop, dynamic script parameters/events, DMX input | `scriptAtLaunch` (**cfg**); `universe`, `startChannel` (**rw**) | `load`, `stop`, `setParam`, `trigger`; events `scriptEvent`, `scriptParamFeedback` | wasm3, filesystem, optional DMX listener |
| `USE_BEHAVIOUR`, `/behaviours/behaviourN` | watches another registered parameter and launches an action | parameter path, comparator/value/time, repeat policy, action/value (**cfg**); `valid` (**rw**) | event `CommandLaunched` | direct pointer to target parameter; root command routing |
| `USE_SEQUENCE`, `/sequence` | compiled placeholder | only inherited fields | none | setup/update/clear are empty |
| `USE_DUMMY`, `/dummies/dummyN` | schema/control test component | two bools, int, float, string (**rw**) | none | none |
| `USE_DISPLAY`, `/display` | M5StickC logging display | inherits IO fields if setup is invoked through base behavior | custom `log` | M5GFX; only M5StickC type exists |
| `USE_SERVO`, `/servo` | positional servo | `pin`, `position` (**cfg**) | none | Arduino Servo |
| `USE_STEPPER`, `/stepper` | position/speed/acceleration stepper control | pins, acceleration, speed (**cfg**); `position` (**ro**, although change handler treats it as a command target) | none | FastAccelStepper |
| `USE_DC_MOTOR`, `/motor` | intended H-bridge DC motor component | intended enable/direction pins (**cfg**) and speed (**rw**) | none | dormant/incomplete: required fields and methods are absent from the header at the pinned commit, so enabling it is not a supported build |
| `USE_FLOWTOYS_CONNECT`, `/Flowtoys Connect` | Flowtoys RF24 sync bridge with DMX mapping and pairing | DMX address, page/mode/adjust, color controls/LFOs, power press (**cfg/rw**) | none | RF24 over SPI, button/GPIO/DIP collaborators; packed proprietary sync/invite structs |
| `USE_RF24`, no tree path | RF24 component placeholder | none | none | both `RF24Component.h` and `.cpp` are empty; Flowtoys uses the RF24 library directly |

## Public HTTP and discovery behavior

- `GET /?HOST_INFO` returns OSCQuery host metadata and extension flags.
- `GET /` returns the component/parameter hierarchy. `config=0` suppresses
  parameters tagged `Config`.
- `GET /firstrun` serves a built-in gzip bootstrap page when files support is
  enabled.
- `POST /uploadFile` accepts file uploads; OTA upload handling is compiled when
  `USE_OTA` is set.
- Unmatched GET paths resolve from the active filesystem, first directly and
  then below `/server`, with gzip alternatives and a first-run redirect for
  missing editor assets.
- WebSocket `/` accepts binary OSC packets and emits binary OSC feedback. Text
  frames are logged but do not route commands. Debug logs use JSON text frames.

## Known compatibility hazards

These observations are compatibility inputs, not behavior V2 must reproduce:

- V1 persisted settings and wire packets have no explicit version.
- Settings save clears the NVS namespace before writing the replacement blob;
  it is not power-loss atomic, and temporary serialization allocations are not
  freed on every load/error path.
- The serial tokenizer does not accept negative numbers as numeric values,
  treats comma as the only argument separator, and has an off-by-one overflow
  risk after the 512-byte input buffer fills.
- Serial feedback paths use a leading slash and underscore-separated component
  path (for example `/leds_strip1.brightness`), unlike serial input paths.
- ESP-NOW parses several length fields before fully proving that the referenced
  bytes exist and serializes native little-endian 32-bit `int`/`float` values.
- Playback metadata has no version, pixel format, frame size, or checksum. With
  the active `PLAYBACK_USE_ALPHA` flag, color bytes are native `Color` memory
  order: alpha, red, green, blue.
- Component managers and resources are compile-time conventions rather than
  validated ownership contracts; unrelated components often access singleton
  internals directly.
- Sequence and RF24 are empty placeholders, while DC motor is incomplete.

V2 preserves useful public behavior through explicit adapters and importers. It
does not preserve the unsafe parsing, unversioned formats, undocumented memory
layout, or accidental coupling listed above.
