# Verification status — v2.1.1

## Automated checks performed

- Universal `Si468x` driver commit used by this application:
  `b671d372a4fe5bb4df493ea6147c215577b25dc2`.
- Vendor `Si468x.h` raw SHA-256:
  `A894CF940CBD143D9C0917BF68C3F335370AF28FC06E9D074F0EEA91545FBF6E`.
- Host scheduler policy test: generation rollover/matching, three bounded
  retries, linear backoff, cancellation reset and `millis()` wraparound.
- Host JPEG tests with GCC C++11, `-Wall -Wextra -Werror -pedantic`:
  baseline grayscale, YCbCr 4:4:4/4:2:2/4:2:0, a complete 320×240 baseline
  4:2:0 decode, DRI/RST, premature EOI,
  malformed DHT, baseline multi-scan classification, oversized dimensions,
  APP0 progressive SOF2 rejection and PNG RGBA fixture structure.
- The display-sized progressive fixture is 320×240, begins `FF D8 FF E0`, has
  valid SOF2/EOI and is approximately 12 KiB. It is rejected before any TFT
  output; it is not the unavailable station object with hash `47083CD0`.
- PlatformIO release build with `espressif32@6.9.0`: success for `esp32dev`
  without PSRAM. Reported static usage: 48,416 bytes RAM (14.8%) and
  3,405,193 bytes flash (82.5%). The persistent 50 KiB MOT buffer and 76,800
  byte shared decoder arena are allocated at runtime and therefore are not
  included in that static RAM number.
- Clang is not installed in this environment: **NOT VERIFIED**.

The generated `.pio/build/esp32dev/firmware.bin` is a successful development
build of 3,405,808 bytes with SHA-256
`9F1888F6B509B9C1E37A8892AA391B00A51F635086F042300A7DEF960E3A5A21`.
It is not copied into `Release/` or represented as a hardware-validated release.
`Release/firmware_v2_0.bin` remains an unchanged historical image.

## Hardware validation still required

1. GPIO12 unconnected/AUTO: verify `hw=Polling`, no IRQ edges, normal DAB/FM,
   manual retune and responsive UI.
2. GPIO12 connected to INTB: verify IRQ-first completion with bounded safety
   polling; a nonzero `ctsPoll` count alone is not a failure.
3. Rapid DAB retune during DSRV/SLS: verify data STOP, audio STOP, at least
   200 ms from the last confirmed STOP, then START/TUNE; no stale multiplex
   metadata or slideshow is published.
4. Real tuner CTS loss: verify bounded data/STOP retries, shared GPIO17 reset,
   `hostAbort` before PRECHECK, successful image upload, and controllable UI if
   recovery fails.
5. Display many baseline JPEG/PNG slides and the 320×240 progressive fixture.
   Progressive rejection must not change backlight or TFT GRAM. Confirm PNG
   alpha appearance and encoder responsiveness during validation/render.
6. Repeat FM/RDS, DAB service restore, FM↔DAB switch, presets, light sleep/wake
   and cold boot regression checks.

## Expected diagnostic sequence examples

```text
[RADIO/RESET] physical reset complete hostAbort=-11 generation=...
[RADIO] PRECHECK result=0 image=...
[RADIO/CTS] late host service gap=... us afterDeadline=... us result=0 (CTS-ready time unknown)
[DAB/STOP] unconfirmed result=-4 retry=1/3 backoffUntil=...
[SLS/JPEG] tid=... size=... hash=... coding=UNSUPPORTED_PROGRESSIVE_JPEG ...
```

`hostAbort=-11` means an old `WaitCts` was cancelled; `hostAbort=0` means the
driver was already idle. Host lateness diagnostics do not prove a tuner fault.
