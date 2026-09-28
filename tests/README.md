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
  -I tests/stubs -I src tests/test_jpeg.cpp src/JPEGdecoder.cpp `
  -o test_jpeg.exe
./test_jpeg.exe
```

`test_scheduler.cpp` exercises the actual generation, retry/backoff, phased
deadline, continuous-DSRV fairness, FM work-priority, RF/display-filter,
one-shot time-sample and debug-throttling helpers used by the firmware.
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
