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
  4:2:0 decode, and progressive grayscale/4:4:4/4:2:2/4:2:0 decodes including
  DRI/RST, non-MCU-aligned 17×13 dimensions, complete 320×240 output and a
  320×320 progressive/baseline pair reduced to matching 160×160 output.
  Corruption coverage includes premature EOI, malformed DHT/DQT, skipped
  progressive refinement levels, invalid restart sequence, baseline multi-scan
  classification and oversized dimensions.
- The display-sized progressive fixture is 320×240, begins `FF D8 FF E0`, has
  valid SOF2/EOI and is approximately 12 KiB. Its decoded RGB565 output is
  pixel-identical to the matching baseline fixture. It is not the unavailable
  station object with hash `47083CD0`.
- PlatformIO release build with `espressif32@6.9.0`: success for `esp32dev`
  without PSRAM. Reported static usage: 48,696 bytes RAM (14.9%) and
  3,409,393 bytes flash (82.6%). The persistent 50 KiB MOT buffer and 76,800
  byte shared decoder arena are allocated at runtime and therefore are not
  included in that static RAM number.
- Clang is not installed in this environment: **NOT VERIFIED**.
- The installed MinGW GCC lacks ASan/UBSan runtime libraries: sanitizer run is
  **NOT VERIFIED**.

The generated `.pio/build/esp32dev/firmware.bin` is a successful development
build of 3,410,016 bytes with SHA-256
`6FAA2FD85991D70B490BFED53FED10E364B42C3FB66C293BC161E43444F81584`.
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
   Also exercise the 320×320 progressive fixture and manually request an
   unsupported/corrupt slide. Confirm complete 160×160 square output, that a
   failed object closes the "Loading slideshow" overlay, acceptable decode
   time, no CTS timeout caused by rendering, PNG alpha appearance and encoder
   responsiveness during validation/render.
6. Repeat FM/RDS, DAB service restore, FM↔DAB switch, presets, light sleep/wake
   and cold boot regression checks.

## Expected diagnostic sequence examples

```text
[RADIO/RESET] physical reset complete hostAbort=-11 generation=...
[RADIO] PRECHECK result=0 image=...
[RADIO/CTS] late host service gap=... us afterDeadline=... us result=0 (CTS-ready time unknown)
[DAB/STOP] unconfirmed result=-4 retry=1/3 backoffUntil=...
[SLS/JPEG] tid=... size=... hash=... coding=SUPPORTED_PROGRESSIVE_JPEG ...
[SLS/JPEG] render=OK size=... lastMCURow=...
```

`hostAbort=-11` means an old `WaitCts` was cancelled; `hostAbort=0` means the
driver was already idle. Host lateness diagnostics do not prove a tuner fault.
