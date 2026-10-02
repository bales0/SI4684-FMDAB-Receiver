# Host regression tests

Generate deterministic image fixtures (requires IJG/libjpeg `cjpeg` and
PowerShell `System.Drawing`):

```powershell
./tests/generate_fixtures.ps1
```

Compile and run with GCC C++11 from the repository root:

```powershell
g++ -std=c++11 -Wall -Wextra -Werror -pedantic `
  tests/test_scheduler.cpp -o test_scheduler.exe
./test_scheduler.exe

g++ -std=c++11 -Wall -Wextra -Werror -pedantic `
  tests/test_fm_features.cpp -o test_fm_features.exe
./test_fm_features.exe

g++ -std=c++11 -Wall -Wextra -Werror -pedantic `
  tests/test_dab_service_switch.cpp -o test_dab_service_switch.exe
./test_dab_service_switch.exe

g++ -std=c++11 -Wall -Wextra -Werror -pedantic `
  -I tests/stubs -I src tests/test_jpeg.cpp src/JPEGdecoder.cpp `
  -o test_jpeg.exe
./test_jpeg.exe
```

`test_scheduler.cpp` exercises CTS host-starvation classification and recovery
counting, AUDIO_INFO delays, tune-busy backoff constants, service-list
generations, label quality, the existing generic retry/backoff, phased
deadline, continuous-DSRV fairness, FM work-priority, RF/display-filter,
one-shot time-sample and debug-throttling helpers used by the firmware.
`test_fm_features.cpp` covers regional FM wrapping, AF validation,
deduplication, quality hysteresis and wrap-safe sweep timeout, RDS CT/MJD
parsing and chronological confirmation across minute/day/month/year rollover,
RDS versus RBDS PTY lookup, PI/frequency
station identity, DAB audio-service boundary selection, 38-channel wrap and
short/long Rotary 1 press suppression. It also locks down Rotary 2 routing:
FM AUTO scan-list/no-list behavior, DAB global-list/current-mux fallback,
closing the slideshow wait view, and exact/unique SID-component matching.
`test_dab_service_switch.cpp` covers serialized data/audio teardown, rapid
request supersession, wrap-safe bounded backoff, both settle intervals,
metadata-trigger deduplication, audio-only deferred SLS retries and SLS context
ownership, including audio-PAD SLS without a separate data service.
`test_jpeg.cpp` runs the application JPEG parser and
decoder against baseline grayscale, 4:4:4, 4:2:2, 4:2:0 and restart-marker
fixtures, including a complete 320×240 baseline 4:2:0 decode. Progressive
coverage includes grayscale, 4:4:4, 4:2:2, 4:2:0, DRI/RST, 17×13 edge blocks
and complete 320×240 APP0/SOF2 output. A 320×320 pair verifies identical
baseline/progressive 50%-scaled 160×160 output. Each valid progressive fixture
is compared pixel-for-pixel with the matching baseline decode. The suite also
covers premature EOI, malformed DHT/DQT, skipped refinement levels, a wrong
restart marker, oversized/four-component frames and baseline multi-scan
classification.

The PNG alpha fixture is checked structurally here and is decoded by PNGdec in
the PlatformIO firmware build/runtime path. Pixel appearance, MOT traffic,
actual TFT fading and radio timing still require the hardware plan in
`VERIFICATION.md`.
