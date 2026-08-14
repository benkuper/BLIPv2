# Milestone 1 verification evidence

This record belongs to the repository commit containing it. Tests were run on
2026-08-14 with ESP-IDF `v6.0.2` at
`7101770dc6db2667b3c477cc31365dd1acd6db4e`, clang-format 18.1.8, and the
committed target-specific dependency locks.

## Host and static checks

- Foundation validation: 10 checks passed.
- Bootstrap validation: 5 checks passed.
- Resource manifest validation: 4 checks passed, including two expected
  conflict failures with both claimants named.
- C++ host suite: 3 CTest executables passed, containing 19 logical tests.
- BLIP-owned C++ formatting and warning-as-error builds passed.

## Reproducible clean builds

Each target was built from scratch in two separate directories. The application
images matched byte-for-byte.

| Target | Application image | SHA-256 | ESP-IDF linked size | Static memory regions used |
| --- | ---: | --- | ---: | --- |
| ESP32 | 113,456 B | `d475d87c2943b2593733ff9709363cb8941de58430b53a5c4bb397a44a9bc88d` | 113,345 B | IRAM 36,987 B; DRAM 13,286 B; RTC slow 64 B |
| ESP32-S3 | 132,000 B | `a71a8db9003682f5ba7a87e970304a574b44b3357878e41805852b1caca2d835` | 131,880 B | DIRAM 40,400 B; IRAM 16,384 B; RTC slow 36 B; RTC fast 24 B |
| ESP32-C6 | 114,208 B | `65645b293f2a238c2abfed77262280d5234f24bc3c700973907d5433f64351ce` | 113,902 B | DIRAM 40,802 B; LP SRAM 24 B |

The application-image value is the generated `.bin` file length. The linked
size and memory regions are from ESP-IDF's `idf_size.py --format json2` report.

## Gate disposition

The implementation and local build portions of work packages 1.1–1.5 are
complete. Gate A is not claimed complete: flashing/boot timing, 100 cold boots,
runtime heap reserve, and task stack high-water marks require physical ESP32,
ESP32-S3, and ESP32-C6 reference boards. The expected readiness marker is:

```text
BLIP_V2_BOOTSTRAP_READY schema=1 registry=1 scheduler=ready resources=ready target=<target> idf=<version>
```

CI results attached to the containing pushed commit are the commit-bound
evidence for `A-01`. The remaining hardware rows are deliberately open.
