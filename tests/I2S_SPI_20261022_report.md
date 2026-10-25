# I2S framing and packet-mode DSRV CID repair — 2026-10-22

Implemented locally after user approval from
`Specification/Codex_zadani_SI4684_I2S_SPI_2026-10-22.md`.
Starting HEAD: `1fa47b36499d79b6e90778b6cbaedda8ca57d0b7` (matches specification).
No commit, GitHub operation, upload or receiver measurement was performed.
The pre-existing `_usertesting/` inputs were read without modification.

## Changes and rationale

- `src/i2s_audio.h`: single named format constant `I2S_AUDIO_FORMAT_STANDARD=0x1870`.
- `src/si4684.cpp`: fresh DAB/FM boot writes typed SELECT=0x8000, FORMAT=0x1870,
  SAMPLE_RATE=32000/44100/48000 in that order before enabling digital pins and
  before tuning. Reads all three properties back with bounded 250 ms timeouts;
  readiness requires both successful Result and exact expected value for each.
  The later FORMAT=0x1600 write is removed entirely; 0x1710/0x1711 are retained.
- `src/dab_spi_probe.h`: same-SID, zero-extended 16-bit CID fallback for TMID=3
  endpoints only, and only for source=0, at SPI/SLS routing. Exact 32-bit
  Endpoint::matches, command IDs, probe state machine and audio PAD matching
  are unchanged. Exact full-CID SPI/SLS matching still permits source=0/1;
  source=2/reserved is rejected. SPI retains priority over audio, then SLS.
- `tests/test_i2s_audio.cpp`: compile-time format assertion; retains real NVS,
  all mode mappings, defaults, pin masks and menu tests.
- `tests/test_i2s_integration.py`: compiles actual production configuration
  against a fault-injecting mock. Tests OFF and every active rate, repeat init,
  cold/reused images, all three write failures, all three read failures with
  expected output values, all three mismatches and stale readiness. Checks
  analog/INTB preservation, diagnostics and existing boot/menu wiring.
- `tests/test_dab_spi_probe.cpp`: captured MUX 11A SID/CID, unchanged vendor
  DSRV parser into route, every source byte, wrong SID/CID/high bits/TMID,
  invalid endpoints, separate packet SLS, shared-SID audio PAD/DLS, routing
  priority, full START/STOP ownership, cancellation/late data/CleanupFailed.
  Existing cleanup retry/window/wrap tests remain.
- `tests/README.md`: links current report and test coverage in existing workflow.
- This report and `I2S_SPI_20261022.patch`: handover and exact review diff.

## Running-image reuse / failure behavior

AN649's pre-first-tune restriction is also documented at the vendored typed
DIGITAL_IO_OUTPUT_FORMAT declaration. An ESP32-only restart cannot establish
whether an already running receiver has tuned. Therefore reused DAB/FM images
receive no I2S SELECT/FORMAT/RATE writes. All three are read and compared with
current selection instead. Existing boot/reset/TFT logic is retained.

If any write/read fails, or any readback differs, readiness remains false.
The existing subsequent PIN_CONFIG write applies 0x0001 (analog/polling) or
0x8001 (analog/INTB); digital bit 1 is disabled. If this pin write itself fails,
existing begin() fails rather than reporting success. OFF skips all digital
property access and uses the same analog/INTB masks.

A reused old 0x1600/0x1800 format or a mismatching sample rate deliberately
continues with analog only. A full receiver power cycle or the existing
reset/reload path is needed to apply the requested digital configuration.
A matching reused image is accepted without risky post-tune format writes.
Cold DAB/FM mode changes and I2S menu changes use the existing reset/reload path;
menu/NVS/defaults, analog volume, IRQ selection and runtime scheduler are untouched.
No I2S readback is added to Update() or an active reception loop.

## Validation results

All commands ran from repository root; host compiler was
`C:/msys64/ucrt64/bin/g++.exe`, flags `-std=c++11 -Wall -Wextra -Werror -pedantic`.

PASS: compile and execute each of:
`scheduler`, `fm_features`, `dab_service_switch`, `dab_user_application`,
`dab_spi_discovery`, `dab_spi_probe`, `dab_packet_inventory`, `dab_dsrv_header`,
`dab_diagnostics`, `i2s_audio`.
Commands (replace NAME with each listed suffix):

```powershell
g++ -std=c++11 -Wall -Wextra -Werror -pedantic tests/test_NAME.cpp -o .pio/test_NAME.exe
./.pio/test_NAME.exe
# i2s_audio additionally needs -I tests/stubs/nvs and src/nvs_storage.cpp.
python tests/test_i2s_integration.py C:/msys64/ucrt64/bin/g++.exe
python tests/test_dab_packet_runtime.py C:/msys64/ucrt64/bin/g++.exe
python tests/test_dab_xpad_integration.py C:/msys64/ucrt64/bin/g++.exe
pio run -e esp32dev
```

PASS: all three Python integration suites, including existing production
inventory/discovery/generation/UART/EPG and XPAD/DSRV diagnostic regressions.
Updated packet and I2S fixtures were recompiled and rerun after their last edit.
PASS: firmware esp32dev release build. DEBUG is runtime gated; this same binary
contains both diagnostic and normal paths. Actual DEBUG-on/off timing is HW unverified.
The only build warning was the existing TFT_eSPI missing TOUCH_CS warning.

Build size: static RAM **62,272 / 327,680 B**, flash **3,458,437 / 4,128,768 B**.
No new runtime heap allocation, large buffer, DSRV log, delay or busy loop was added.
Runtime free DRAM and audio continuity must still be measured on hardware.
Application file `.pio/build/esp32dev/firmware.bin`: **3,459,056 bytes**,
flash offset **0x10000** (preserves existing NVS).
SHA256: `57DCEBA75447B61E30DC14B6F05E57DEC3DAAD5EEF7EF4E8107C787DB7A4CBB6`.
Do not confuse the application with generated `receiver-complete.bin` at 0x0,
which replaces NVS settings.

## Expected diagnostic examples (illustrative, not new HW captures)

For each selected active rate, successful verification includes:

```text
[I2S] readback property=0x0200 result=0 expected=0x8000 actual=0x8000
[I2S] readback property=0x0202 result=0 expected=0x1870 actual=0x1870
[I2S] readback property=0x0201 result=0 expected=0xBB80 actual=0xBB80
[I2S] verified; digital output ready
```

Rate values: 32 kHz=0x7D00, 44.1 kHz=0xAC44, 48 kHz=0xBB80.
Expected BCK at 64*Fs: respectively 2.048, 2.8224 and 3.072 MHz; not measured.
Mismatch, including reused old-format firmware:

```text
[I2S] readback property=0x0202 result=0 expected=0x1870 actual=0x1600
[I2S] verification failed; digital pins disabled, analog DAC retained
```

On GET failure, result is nonzero, actual is initialized to zero (unless the
transport wrote another value), readiness remains false even if actual=expected,
and the same analog-only failure line follows. A failed SET prints its property,
value and result followed by `configuration failed; ...` instead.

On Polish MUX 11A, after explicit `DEBUG SPI` (also enables DEBUG):

```text
[DAB/SPI/TEST] START result=0 reason=00; no SLS STOP issued
[DSRV/ROUTE] route=3 src=0 sid=E231B3FE cid=0000C00B dscty=60 bytes=512 ...
[DAB/SPI/TEST] STOP result=0 reason=00 attempt=1
```

`route=3` means Route::Spi. During observation, SPI counters should grow without
feeding legacy SLS/DLS; START/STOP still send full CID 01F2C00B. Baseline remains
5 seconds, observation 30 seconds, bounded cleanup unchanged. No permanent SPI
service, payload dump or MOT/EPG decoder is introduced. Presence still requires
validated UAType=0x007, independent of probe counters or DSCTy alone.

## HW evidence and remaining receiver checks

Provided reference A/B files under `_usertesting/moje-pliki.zip` support Andrzej's
original observations: old routing ignored 460 events; reference patch routed
440 SPI events with zero ignored, without disrupting completed SLS/audio as
reported in the assignment. These counts describe events, not decoded SPI objects.
The reference `_usertesting/si4684.cpp` also supplied the 0x1870 hardware result;
its unchecked readiness and second format write were not copied.
**The new implementation has NOT been tested on physical hardware.**

Verify PCM5102A on OFF/32/44.1/48 kHz, cold DAB/FM, DAB-FM-DAB, menu rate reload,
ESP32-only restart with matching and mismatching running-image properties, and
confirm clean audio plus measured BCK/LRCK. Verify analog audio and INTB remain
available on OFF/failure. Repeat DEBUG off/on, infotext, new JPEG/PNG completion,
EPG negative/positive indication, same-mux station changes and cross-mux reset.
Run manual SPI probe on 11A; confirm short-CID SPI counts, full-ID START/STOP,
new slideshow completion, no added CTS timeouts/overflow/UART drops, and safe
cleanup when switching station/mux. Runtime free/largest heap remains HW unverified.

The repository already ignores untracked tests under `tests/`. The patch includes
updated ignored test sources as complete new-file additions relative to HEAD,
so they are not silently omitted by ordinary git diff. No gitignore/index changes
were made. Existing local test stubs/dependencies remain prerequisites for the
full host suite as documented in tests/README.md.
