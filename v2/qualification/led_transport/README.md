# LED transport qualification harness

This disposable ESP-IDF application measures transport mechanics before a
driver is admitted into production firmware. It never imports a BLIP component
and emits newline-delimited JSON so results can be captured without network
access. The workstation therefore remains connected to its normal Wi-Fi.

The current harness exercises native RMT with every allocatable lane, RMT DMA
where the SoC advertises it, SPI one-wire expansion with DMA disabled/enabled,
and native clocked SPI frames. It reports unsupported lane counts instead of
extrapolating them. Target parallel engines and multiline wiring are reported
as fixture-required until a pin manifest and external capture fixture are
provided; timing correctness cannot be inferred from submission completion.

Build with the pinned ESP-IDF and explicit disposable output pins:

```powershell
idf.py -C v2/qualification/led_transport -B build/led-qual-c6 `
  -DIDF_TARGET=esp32c6 -DBLIP_QUAL_RMT_GPIO=20 `
  -DBLIP_QUAL_SPI_DATA_GPIO=19 -DBLIP_QUAL_SPI_CLOCK_GPIO=18 build
```

Flashing this application erases part of the running BLIP application. A
physical run must first capture every flash range the qualification image will
overwrite (a full-flash backup is preferred), then restore and verify those
exact bytes before the run is considered complete. `BLIP_LED_QUAL_DONE` is the
capture end marker. External evidence must state the probe, sample rate,
voltage, pin map, load mode, and decoder/error criteria.

`run_hil.ps1` flashes, captures through the completion marker, and restores the
supplied overwrite-range backup in an unconditional `finally` block. It then
uses esptool verification before reporting success.

`summarize_results.py` rejects incomplete captures and generates the compact
JSON record plus a filtered raw serial log used as append-only evidence.
