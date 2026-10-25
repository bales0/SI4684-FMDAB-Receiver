# DAB SPI component scan: implementation and verification

## Latest panel simplification and station-name correction

The user-approved replacement removes all four-viewport clipping. Main-widget
render helpers now draw once and mark a shared changed flag. ProcessDAB consumes
that flag after the dynamic fields and repaints the floating panel once, with
volume above slideshow loading when both are requested. No RSSI timestamp
gates the repaint. Unchanged widgets do not continually repaint the panel, and
the overlay itself does not set the flag. RSSI/quality and all dynamic fields
continue updating. This supersedes the clipping section below.

A CurrentServiceInfo reply for old active audio could finish after setService
cleared PStext for a new selection. Previously its check against old active
SID alone allowed it to restore the old label. Metadata acceptance now requires
no pending switch and matching reply/active/requested full SID/CID. A separate
label-valid flag is cleared on selection/reset/retune and set only by accepted
service metadata. ShowPS retains the selected target label through audio START
completion until the current label is ready, for both rotary selection routes.

Six host suites passed, including new stale-name/metadata regressions. A host
mock using the actual dirty-flag and final-composition source blocks passed:
three/four field updates coalesce into one overlay redraw, idle loops trigger
none, changes do not need an RSSI tick, volume wins over loading, and menus
suppress panel repaint. This is not a simulation of the TFT bus or flicker.
The ESP32 build succeeded: RAM 62,232 bytes (-8), flash 3,454,297 bytes
(-1,816), compared with the preceding clipping build. Current app SHA256:
`EB5F4CB2A1B52DD19AD678B411DFAF9A4857C7596A19EDA1AC828C440C607B37`.
No upload was performed. Check rapid Rotary1/Rotary2 selection and overlay
flicker on the receiver using the updated tests/README.md procedure.

## Earlier display correction (clipping superseded above)

EPG now uses the same loaded FONT16 as the codec, in the existing
x=58..115/y=50..63 cell, retaining cyan/grey and no suffix. EID/SID restore
crops are disjoint (EID y=104..118, SID y=119..135), both 40 pixels wide,
so EID cannot erase the top of unchanged SID. A check against actual VLW
glyph metrics validated all 65,536 hexadecimal IDs and E/P/G bounds.

Intersecting dynamic fields keep updating while a floating panel is open.
Their text, sprite and background writes are clipped to four viewports outside
the panel's x=25..294/y=46..95 rectangle. The viewports use absolute screen
coordinates and reset to the full display after each drawing operation.
Field caches update once, after drawing, and overlay close already rebuilds
the main display. In particular, visible RSSI and quality-bar portions remain
live; the fields are not paused. The existing overlay repaint path remains.

Validation covered the outside/inside partition for all 76,800 screen pixels,
including live quality-bar rows 96..100. Six host radio suites passed, and
the final ESP32 build succeeded. Static RAM remains 62,240 bytes; flash is
3,456,113 bytes. Current application SHA256 (superseding hashes below):
`F5E1E7B5FDB1E7524613C22272DD52B8B67883856804217A1E49A14E8DF71BA0`.
Hardware display/performance validation remains pending; no upload was done.

## Subsequent EPG indicator and continuous detection update

The sections below describe the original discovery-fix batch. A subsequent
user-authorized change adds the plain `EPG` indicator in `src/gui.cpp`, with
cyan/grey foreground colors from the slideshow artwork and no suffix signs.
It occupies x=70..103, y=50..63, below the codec and above PR. It is hidden in
FM and repaints only on presence/mode change or full display reset.

The driver now repeatedly schedules a fresh low-priority UA pass 60 seconds
after completion, including failures, with DEBUG either on or off. Context
changes invalidate immediately; routine refresh retains the previous presence
while collecting new replies. Detection has no startup expiry. Probe activity,
switching and loss of lock postpone automatic refresh. All candidates count
towards advertised presence, even when selection is ambiguous; a dim indicator
means presence is not confirmed and does not distinguish unknown from absent.
Actual EPG programme decoding remains future work.

The additional refresh-cycle tests pass for 100 successive cycles, deadline
wrap, postponement and cancellation. All six host suites and the ESP32 build
pass. Current static RAM is 62,240 bytes (+16 bytes for this update), flash
3,454,265 bytes (+412). Current application SHA256:
`090983AFFFCBAF3A6C50C82711FDF79B410E2F163D095C75D017C79184466B46`.
The firmware has not been uploaded; receiver/rendering checks remain pending.

## Scope and changed files

This batch changes `src/si4684.cpp`, `src/si4684.h`,
`src/dab_spi_discovery.h`, `tests/test_dab_spi_discovery.cpp`,
`tests/README.md`, `Serial_protocol.md`, and this report. Earlier uncommitted
work is retained. The existing external change in `src/language.h` was not
edited. No flash/upload was performed.

## Original defect and corrected completion

The inspected source already waited for all component identities in the SPI
selector. The supplied log ordering therefore did not prove that the final
response had been skipped: per-component UA diagnostics are copied and emitted
over several later scheduler turns. `refreshDabSpiDiscovery()` could publish
the result while the final component's detailed output was still pending.
Completion also lacked an explicit count of committed responses separate
from the incremented scheduling cursor.

The production `dab_spi::Scan` now records enumeration validity, request
generation, total/scheduled/processed/failed counts and an outstanding response
flag. A response is committed only after the read and UA validation finish;
command/read/parser/start failures settle a failed request. Publication requires:

```text
inventory valid
scheduled == total
processed + failed == total
no outstanding scan response
no active ComponentInfo command
no pending detailed UA or component-list output
```

The scheduler then queues the scan summary, candidate count and final result
on separate paced turns. The selector preserves its existing deterministic
policy: prefer a unique SPI component of the current audio SID, otherwise a
unique ensemble candidate; ambiguous candidates remain ambiguous. Any failed
identity makes the result `discovery incomplete`. Profile remains `unknown`.

## Complete component enumeration and generations

The existing fixed side table already retained every CID: at most 32 services
with 14 components each, 448 full 32-bit CIDs. The legacy UI/audio station
structure still retains its first component. The firmware now uses a tested
bounded CID-retention helper after validating the complete raw service list.
It rejects insufficient input or storage before copying. Capacity errors
invalidate discovery and report incompleteness, retaining the legacy station
list. No new component table or heap allocation is introduced.

`[DAB/SVCLIST]` enumerates each SID/count and every CID once for a changed
validated topology or explicit DEBUG activation. Identical periodic lists do
not restart the scan. Each side-table entry is queried once per pass, with the
existing 500 ms query gap and low scheduler priority.

The scan has its own generation, independent of periodic service-list receipt
generations. A topology change, DEBUG restart, retune or reset invalidates old
responses, including a response for the same SID/CID from an earlier pass.
Ordinary audio service switching retains the ensemble identities and evaluates
selection only after the new audio context stabilizes; scan queries remain
behind service switching and DSRV work.

The UA parser, DSRV decoding/routing, SLS MOT collector, 51,200-byte image
buffer, image cache, audio commands and UI behavior were not changed by this
batch. No SPI/MOT Directory/EPG decoder or EPG arena was added.

## Build, memory and host verification

`pio run -e esp32dev` succeeded. Static RAM is 62,224 bytes, an increase of
24 bytes from the preceding 62,200-byte build. Flash usage is 3,453,853 bytes,
an increase of 1,280 bytes. No dynamic storage was added. The existing
TFT_eSPI TOUCH_CS warning remains.

All six host suites compiled with GCC C++11,
`-Wall -Wextra -Werror -pedantic`, and passed:

- DAB SPI discovery, including the new completion/CID retention regressions;
- DAB user applications, including 65,536 length combinations;
- DAB SPI probe/router;
- scheduler;
- FM features;
- DAB service switching.

The added tests exercise the same completion and CID-retention helpers used
by firmware. They cover acceptance scenarios A-E: a final outstanding request,
last-component SPI, three retained/queried components for one SID, a failed
request and stale generation rejection. They also check pending diagnostic
output, no duplicate commits, 448 requests, empty/invalid inventories, generation
wrap and retention bounds. They do not emulate the full radio transport/UART
or establish hardware coexistence. `git diff --check` passed.

Firmware artifact: `.pio/build/esp32dev/firmware.bin` (application at `0x10000`).
SHA256: `FFB86E117B6067310D2DAAF6B7B6FE96C4FDC1AAA68AE6A13D166B0773B3542D`.

## Expected diagnostics

Illustrative successful scan; actual IDs/counts depend on the multiplex:

```text
[DAB/SVCLIST] SID=00001234 components=3
[DAB/SVCLIST]   CID[0]=01FE0001
[DAB/SVCLIST]   CID[1]=02FE0002
[DAB/SVCLIST]   CID[2]=03FE0003
... UA results for CID[0] and CID[1] ...
[DAB/UA] SID=00001234 CID=03FE0003 label="" lang=0 count=1 bytes=4
[DAB/UA]   type=0x007 (SPI) len=0
[DAB/UA] scan complete services=1 components=3 processed=3 failed=0
[DAB/SPI] candidates=1
[DAB/SPI] selected service audio=00001234 SID=00001234 CID=03FE0003 profile=unknown
```

Zero candidates after all successful responses:

```text
[DAB/UA] scan complete services=17 components=17 processed=17 failed=0
[DAB/SPI] candidates=0
[DAB/SPI] not available audio=0000232F SID=00000000 CID=00000000 profile=unknown
```

One failed response:

```text
[DAB/UA] scan complete services=17 components=18 processed=17 failed=1
[DAB/SPI] candidates=0
[DAB/SPI] discovery incomplete audio=0000232F SID=00000000 CID=00000000 profile=unknown
```

## Receiver verification pending

Hardware acceptance F and actual UART ordering remain to be checked on the
receiver after uploading the application firmware:

1. Tune a DAB multiplex, enable DEBUG, and capture the full scan.
2. Match each `[DAB/SVCLIST]` SID/component count/CID with one UA result or
   explicit query/parser failure. For services with multiple components,
   verify every CID, including the final one.
3. Verify `components == processed + failed`; the summary and final SPI result
   must follow the final UA detail line. With any failure, require
   `discovery incomplete`, never `not available`.
4. Verify identical periodic service lists do not repeat the dump. Toggle
   DEBUG to request a fresh pass. Retune/change the inventory mid-scan and
   verify the new pass has no mixed old/new results.
5. Switch audio stations during discovery; confirm prompt switching, infotext,
   and a newly completed SLS image on the display.
6. Confirm SLS continues assembling during scanning, with DSRV overflow=0,
   CTS genuine=0, recovery=0 and SERIAL dropped=0 under normal reception.
7. Repeat using INTB and POLL/AUTO where available; compare heap stability.

A multiplex that advertises UAType `0x007` is still required to verify actual
SPI discovery and the later controlled coexistence probe on Si4684 FW 6.0.9.
