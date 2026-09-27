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

g++ -std=c++11 -Wall -Wextra -Werror -pedantic -Wno-unused-function `
  -I tests/stubs -I src tests/test_jpeg.cpp src/JPEGdecoder.cpp `
  -o test_jpeg.exe
./test_jpeg.exe
```

`test_scheduler.cpp` exercises the actual generation and retry/backoff helpers
used by the DAB scheduler. `test_jpeg.cpp` runs the application JPEG parser and
decoder against baseline grayscale, 4:4:4, 4:2:2, 4:2:0 and restart-marker
fixtures, including a complete 320×240 baseline 4:2:0 decode. It also covers
premature EOI, malformed DHT/DQT, oversized and
four-component frames, baseline multi-scan classification, and two progressive
APP0/SOF2 fixtures. The larger progressive image is 320×240 and approximately
12 KiB; the test confirms rejection before a single TFT output line.

The PNG alpha fixture is checked structurally here and is decoded by PNGdec in
the PlatformIO firmware build/runtime path. Pixel appearance, MOT traffic,
actual TFT fading and radio timing still require the hardware plan in
`VERIFICATION.md`.
