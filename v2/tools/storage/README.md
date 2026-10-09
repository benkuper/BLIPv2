# Automatic device files

Bulk files use one device service. A mounted board-declared external volume is
preferred; otherwise writes use internal LittleFS. Reads can use an existing
internal file when the mounted external volume has no corresponding file.
Readers and uploads stay on one medium and one generation through completion.
Missing cards do not prevent firmware startup; unknown media is not formatted.
Mount selection currently happens at startup. Live card reprobe remains open.

The public HTTP API requires no password or API key. Relative logical names use
ASCII letters, digits, `-`, `_`, `.`, and `/`, with a maximum of 96 characters.
Each path component must also fit its filesystem's name limit, including the
three-byte physical generation suffix (LittleFS is configured for 64 bytes).
Traversal and encoded/absolute paths are rejected. The default file bound is
16 MiB; capacity can be lower on the selected volume. One writer and four
readers are admitted. A busy updater rejects manual file transfers, and active
manual transfers defer update checks/installs.

| Method | URL | Result |
| --- | --- | --- |
| `PUT` | `/api/files/scripts/<name>` | Stream and commit a script file; does not execute it |
| `PUT` | `/api/files/playback/<name>` | Stream and commit playback data |
| `PUT` | `/api/files/sequences/<name>` | Stream and commit sequence data |
| `GET` | Any of the above | Download the current checked generation |
| `DELETE` | Any of the above | Commit a deletion tombstone; a subsequent GET returns 404 |

Web/server assets go through `PUT /api/web-assets` or the device release service,
so their format and CRC are validated before publication. Factory flashing
preloads internal LittleFS; a checked copy migrates to external media at startup
when available. The full UI is absent from the application image.

Files have two physical `.b0`/`.b1` generations under the storage service's
namespace. Their `BLBF` envelopes are not the raw payload; use the HTTP API or
`AutomaticFileStore` to read/write them. A durable CRC trailer publishes a
generation, including on FAT without overwrite-by-rename. Interrupted uploads
retain the prior valid generation. SDK/card I/O failures do not redirect an
in-progress operation to another filesystem. Factory UI remains an internal
fallback if external media is absent at the next boot.

Run the hardware test on an already provisioned shared network, using the
ESP-IDF Python environment (serial/esptool required):

```powershell
python v2/tools/storage/blip_file_storage_hil.py `
  --device http://DEVICE_IP --port COM_PORT --build build/BOARD_BUILD `
  --flash-log build/FLASH_LOG --mac FLASHED_MAC --expected-medium sd-spi `
  --reset --report build/storage-trial.json
```

It verifies board/network identity, all three namespaces, repeated replacements,
interrupted uploads/downloads, path bounds, empty/deleted files and optional
reboot persistence. It disables periodic release checks for the file test and
restores their original interval. It never changes the PC's Wi-Fi connection.
Browser file listing and playback/sequence consumers remain follow-up work.

With WASM enabled, `blip.wasm/load_file` accepts a logical path such as
`scripts/show.wasm` and returns a queued request ID. Read its usual `completion`
result before calling exports or script actions. The worker validates the stored
generation and module, publishes its declared controls, and loads passively;
uploading or loading a file does not execute guest code. Missing/oversized files
preserve the running module. After buffer reuse begins, an I/O or module
validation failure leaves the worker unloaded. The 16 KiB module bound still
applies. A loaded module is independent of later file replacement/deletion.
There is no boot autostart policy yet.

```powershell
python v2/tools/storage/blip_stored_script_hil.py `
  --device http://DEVICE_IP --port COM_PORT --build build/BOARD_BUILD `
  --flash-log build/FLASH_LOG --mac FLASHED_MAC --expected-medium sd-spi `
  --reset --report build/stored-script-trial.json
```

This adds named loading, exact copied guest results, early/late failure recovery,
96-byte nested paths, repeated replacement and post-reset loading checks. File
validation can take longer than the serial client's execution-completion poll;
this helper allows 60 seconds for file operations. Guest execution budgets are
unchanged. Add `--cancel-validation` on an SD device to require an active file
validation to cancel within one second, retain the current module and reopen
its action admission. File validation checks cancellation between 512-byte reads;
the callback is borrowed for the synchronous scan and consumes no persistent RAM.
