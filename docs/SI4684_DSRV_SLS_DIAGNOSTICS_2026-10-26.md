# MUX 11A DSRV/SLS instrumentation and A/B protocol

26 October 2026. Base HEAD: `e24bc2ac84349dc6f1a9f07ae59b68b4d6a4fd9c`.
The earlier local UART reset/RF change is retained. This work does not flash,
commit, push or write NVS. The new firmware has **HW UNVERIFIED** status.
Earlier user tests concern the earlier firmware, not this instrumentation.

## What the available logs establish

Both ZIPs named in the specification exist under `_usertesting/20261026`.
The following comparison reads the two `e24bc2a_...` members of
`EPG_test_e2ea822_e24bc2a_11A_11B.zip`, without modifying those inputs:

| Observation | 11A PR Trójka | 11B PR Trójka |
|---|---|---|
| Consecutive nominal 30 s CTS-poll increments | 578, 597, 571, 585, 588, 588, 572 | 165, 150, 165, 151, 166, 151, 149, 161 |
| Mean CTS-poll increment | 582.714 | 157.250 |
| DSRV status-event increments over those intervals | 138, 168, 157, 164, 169, 157, 146 | 79, 80, 78, 77, 82, 80, 80, 80 |
| SLS objects with a header and closure inside capture | 4 complete, 8 abandoned | 14 complete, 0 abandoned |
| Additional READY already in flight at capture start | 1, excluded from comparison | 1, excluded from comparison |
| Final open TID | 15800 | 15748 |
| Last packet MOTPATH snapshot | 2757 valid groups, all empty Directory; TID 8CC2, 16 B, repeats 2756 | 473 groups; nonempty Directory F651, 3080 B, 70 objects |
| Overflow | Initially 0, later 3 | 0 |
| Errors/recovery in captured run | 0/0 | 0/0 |

Reproduce the comparison:

```powershell
python tests/analyze_dab_loss_logs.py _usertesting/20261026/EPG_test_e2ea822_e24bc2a_11A_11B.zip
```

The checkpoint-only calculation conservatively reconstructs **10 missing full
512-byte blocks** over 384 expected segments in the 12 closed 11A objects.
The assignment states at least 11; this script does not reproduce that exact
bound from the sparse `Saved` lines, and deliberately does not invent the extra
block. Objects without a final checkpoint may hide further missing segments,
including a shorter LAST block. Neither bound is a measured RF packet loss rate.
For example, TID 15776 ends with 15824/17360 B (three full blocks absent), while
15780 ends with 16848/17360 B (one full block absent). TID 15798 has no deficit
at its last printed checkpoint, segment 16, but is subsequently abandoned.

**Why is the CTS count higher?** The logs prove more CTS completions and more
DSRV events, together with much more observed packet Directory content on 11A.
They do not contain opcode counters synchronized to the IRQ summaries. Thus an
exact historical DSRV/non-DSRV decomposition is unavailable. The new firmware
measures that decomposition; no CPU, RF or chip-FIFO cause is declared proven.
Reported restarts outside these captures cannot be classified retrospectively.

## ACK and preserved-reply audit

[Skyworks AN649, command 0x84 and section 7.7](https://www.skyworksinc.com/-/media/Skyworks/SL/documents/public/application-notes/an649.pdf)
describes a queued service-data block copied to the command reply. Reading the
header and then the full preserved reply requires only one GET command.
`STATUS_ONLY=1` leaves service data queued. ACK clears the interrupt source;
DSRVINT can remain asserted when additional blocks are available. Application
requests remain `ARG1=0x01`: ACK enabled, STATUS_ONLY disabled. No vendor engine,
ACK policy, DSRV burst limit (4), RF cadence, SLS timeout (30 s), image buffer or
collector replacement rule was changed.

The production `readCurrentReply()` issues no new command. It calls the status
callback again; this is a status observation, not a second CTS completion.
`commandAwaitingCts` still admits only the first CTS observation of an operation.
`dab_health::Dsrv` retains its rising-observation sequence and one bounded
verification. Tests cover sticky HIGH, LOW/HIGH during a read, remaining 0/1/N,
empty verification termination, invalid completion backoff and stale epochs.
They model chip responses; they do not independently measure silicon FIFO ACK.

## UART commands and mode lifecycle

Commands are case-insensitive, need CR/LF, and work without `ENABLE=1`:

```text
DSRV MODE A
DSRV MODE B
DSRV MODE C
DSRV MODE D
DSRV STATUS
```

| Mode | Effect |
|---|---|
| A / NORMAL | Normal automatic packet reception, EPG parsing, PAD/SLS/audio; ordinary DEBUG on, verbose off. |
| B / NO_PACKET_SPI_SERVICE | Pause automatic packet START; request STOP of the existing separately owned packet endpoint. Continue PAD/SLS/audio. |
| C / PACKET_DRAIN_NO_EPG_PARSE | Normal packet START, full reads and ACK; skip only `livePayload()` for packet EPG. PAD, including PAD EPG, stays on its established path. |
| D / NORMAL_DEBUG_OFF | Normal production reception/parsing; ordinary DEBUG and verbose off, bounded new aggregate/loss records retained. |

The experiment flag/mode live in RAM and default to normal after ESP32 boot.
All modes continue counting evidence. A–D select a new `modeEpoch`; they do not
change tuner generation, clear the image buffer or reset EPG cache. No endpoint
is relabelled while a command is in flight. Mode B cancels/cleans up through the
existing Probe owner, including a START already in flight; A/C/D wait for that
STOP to finish before normal automatic acquisition restarts. B uses the exact
packet SID/CID from that owner, never an audio/SLS STOP. Shared endpoints,
missing active continuous packet ownership, replay/FM or cleanup failure return
`UNSUPPORTED`. A rejected request preserves the current mode.

`ACCEPTED` means a requested transition, not proof of completed STOP/START.
`DSRV STATUS` prints mode, epoch, actual probe state and DEBUG state. Probe states:
0 Idle, 1 Baseline, 2 StartRequested, 3 Starting, 4 Observing, 5 StopRequested,
6 Stopping, 7 CleanupFailed. Verify **B + probe=0**, and then **A + probe=4** on
return. Failure state 7 requires investigation; never count it as successful B.
Manual `DEBUG SPI` cannot override B. Do not toggle DEBUG during measured D.

## Aggregate records

All new event/action counters saturate at UINT32_MAX. There are no payload
buffers, dynamic allocations, SPI calls, waits or additional commands in the
counter hot path. Full payload reads additionally calculate a bounded content
hash; packet classification checks the existing MOT envelope/CRC. Mode C skips
the EPG Core, not this lightweight diagnostic inspection. Long new UART records
use a fixed 1024-byte stack formatter and enqueue directly, avoiding Arduino
`Print::printf` long-line allocation. Heap/stack health is sampled every 30 s,
not every scheduler pump.

Every 30 s an interval bank is frozen independently of UART headroom. The next
bank immediately begins collecting. One aggregate record or one LOSS/PATH pair
can be queued per 100 ms, with at least 1200 bytes of queue room. Cumulative
totals are sampled when each paced row is emitted, so they can advance after
the frozen interval. A blocked UART can extend the following interval: use
reported `ms`, not an assumed 30 s. `mixed=1` means that interval crossed a mode
epoch; exclude it from A/B comparisons. RATE's mode/epoch describe the frozen
interval end; probe/RF/UART health describe the time of emission.

Illustration only, not a new RF measurement:

```text
[DSRV/RATE] ms=30000 mode=A probe=4 ctsPoll=583 ctsIrq=0 cmdDsrvStart=550 cmdDsrvOk=550 dsrvCts=550 nonDsrv=33 replyEmpty=2 packet=480 pad=68 packetEmptyDir=480 readFail=0 overflowDelta=0 queueMax=3 waitMaxMs=12 drainMaxMs=30 UARTdrop=0 heap=14000 minHeap=13000 stackHWM=2000 lastReadResult=0 RSSIx10=420 CNR=15 FIC=100 lock=1 modeEpoch=1 mixed=0
[DSRV/CMD] op=dsrv delta=550,550,0,0,0,0,0,0,550,0 total=1100,1100,0,0,0,0,0,0,1100,0
```

RATE field definitions:

| Field | Meaning |
|---|---|
| ms, mode, modeEpoch, mixed | Actual frozen duration in ms, selected variant/epoch at end, whether interval crossed a mode change. |
| probe | Actual packet owner state at emission, enum above. |
| ctsPoll, ctsIrq | Sum of classified DAB-runtime CTS completions over exactly the same frozen interval; not SPI status-read counts. |
| cmdDsrvStart, cmdDsrvOk | Accepted Pending DSRV submissions, terminal Ok results. In-flight commands can cross interval boundaries. |
| dsrvCts, nonDsrv | CTS attributed to DSRV and to all other/uncategorized operations respectively. They sum to ctsPoll+ctsIrq. They need not equal DSRV Ok/starts. |
| replyEmpty, packet, pad | Parsed zero-byte replies, source-0 headers, source-1 headers; header observations, not rates or delivered images. |
| packetEmptyDir | Successfully full-read packet groups independently validated as complete empty Directory. |
| readFail, lastReadResult | Failed transport/API full reads in interval, and most recent raw full-read Result code. Length/identity rejection after a successful API read is separate. |
| overflowDelta | Overflow indications observed in DSRV headers in this interval. |
| queueMax, waitMaxMs, drainMaxMs | Maximum reported buffersRemaining; first observed pending-to-start wait; accepted start-to-payload-dispatch completion time. They are observations, not a FIFO size/utilization measurement. |
| UARTdrop | Existing cumulative diagnostic bytes dropped, not RF packets. |
| heap, minHeap, stackHWM | Cached 30 s health sample, ESP32 byte units. |
| RSSIx10, CNR, FIC, lock | Cached RSSI in 0.1 dBuV, CNR dB, FIC quality %, valid AND acquired. |

`DSRV/CMD` emits each operation separately (including LOCAL/UTC/CONFIRM,
metadata, UA, packet/audio/SLS START/STOP, tune and event status). Both vectors
have this order: **Start, Ok, Timeout, Transport, Device, Other terminal,
StartFail, Busy, Poll, Irq**. Start means `startCommand` returned Pending;
Busy/start failure is not a start. Timeout has no CTS. Device errors can have
CTS. Bucket `other` also receives CTS from synchronous runtime operations not
submitted through the asynchronous DAB queue; their starts/terminals are not
fabricated. Boot and FM CTS are excluded from this DAB classification. The
legacy cumulative IRQ counter can include other runtime modes; use the new
frozen vectors for reconciliation.

`DSRV/QUEUE histDelta/histTotal` count headers with buffersRemaining **0, 1,
2–3, >=4**. `fullReads` counts actual full-read API invocations, `transactions`
accepted GET-DATA submissions. Neither `remaining/clearStart` nor this histogram
is a FIFO occupancy percentage. Divide starts by `irqObserved` only as an
aggregate command/event ratio: one interrupt may cover several groups.

`DSRV/PATH` fields use **interval/cumulative** pairs:

| Fields | Observation |
|---|---|
| empty, nonempty; src0..src3 | Header byte count and source. |
| ignored, audio, sls, spi | Unchanged ownership route decision. |
| padSls | Usable source-1 audio/SLS headers selected for legacy SLS decoding; not SPI-UA PAD. |
| physical, state, stale | Physical-error indication, unsupported service-state observation, stale generation/context rejection. Existing `EPG REJECT` retains exact SID/CID/UA/inactive classifications. |
| readOk, readFail | API full-read results; one observation per call. |
| readTimeout, readTransport, readDevice, readOther | Disjoint readFail breakdown; raw Result values retain vendor definitions. |
| lengthMismatch, headerIdentity | Successful read rejected because byte count or SID/CID changed from the short reply. Not counted again as an API readFail. |
| oversize, unsupported | Header cannot fit the shared 4096-byte workspace; or full legacy payload has no supported DLS/MOT format. |
| headerValid, headerCrcBad | Recognized SLS header group CRC observations. Header acceptance policy is preserved; headerCrcBad alone is not a new rejection. |
| candidate, crcOk, crcBad | Recognized SLS body candidates and their mutually exclusive CRC outcomes. |
| stored, duplicate, conflict | Unique stored body segments, validated same-content duplicates, duplicate content/length/LAST conflict. |
| rejectTid, hold, completedRepeat, invalid | Wrong collector TID, published-buffer ownership refusal, repeat of completed TID, invalid length/segment/order/image. |
| packetEmptyDir, packetOther, parseSkipped | Complete empty packet Directory; all other successfully read packet content (including invalid MOT); packet groups withheld from Core in C. |
| identical | Consecutive full payloads with equal SID/CID/tuner generation/length/FNV-32 hash in different accepted GET transactions. Bounded evidence, possible hash collision, not a physical replay count. |
| overflow, verifyStart, verifyEmpty, irqObserved | Header overflow indications, accepted verification starts, empty verification replies, rising DSRV status observations. |
| lossLogDrop | Closed-TID diagnostic queue overflow or formatter bound failure. This drops evidence, never receiver data. |

Only a two-record metadata queue is retained for closed objects. DEBUG off can
fill it before an experiment starts; check lossLogDrop and start from a fresh
boot. No per-segment log was added. Existing `DEBUG SLSV` remains optional and
should stay off for comparisons.

## SLS closure records

Illustration with a single missing segment 16:

```text
[SLS/LOSS] tid=15780 reason=NEW_HEADER total=34 received=33 missing=1 firstMissing=16 missingMask=0000010000000000000000000000000000000000000000000000000000000000 exact=1 bytes=16848/17360 lastSeen=1 ageMs=21000 lastActivityAgeMs=1000 modeEpoch=1/1
[SLS/PATH] tid=15780 overflow=0 physical=0 readFail=0 byteCountMismatch=0 crcBad=0 rejected=0 maxRemaining=2 maxDrainMs=30 padSlsHeaders=34 fullReplyOk=34
```

TID is decimal. `total` is LAST+1 when LAST was validated; otherwise it is only
highest observed segment+1 (or zero for a header with no body). `received` and
`missing` count unique set/unset bits in that range. `firstMissing=256` means
none in that range. The 64 hex characters encode all 32 bitmap bytes in order;
bit 0 of byte 0 is segment 0. `exact=0`/`lastSeen=0` means **a minimum confirmed
hole count**: BodySize remains in `bytes=received/expected` but no constant
segment size or unseen tail is invented. LAST seen with a known BodySize also
permits checking byte completeness independently of the bitmap.

`ageMs` is age since acquiring that collector TID, `lastActivityAgeMs` since last
validated activity, both unsigned/wrap-safe. `modeEpoch=start/end` flags a TID
crossing mode changes; exclude mixed TIDs. Reasons include COMPLETE, NEW_HEADER,
NEW_SEG0, TIMEOUT, INVALID, RESET, TUNE, SWITCH and OTHER. HOLD is recorded as a
group refusal while the completed image owns the single buffer, not a fabricated
abandonment of a new TID. A valid image closes exactly once before metadata reset.

PATH error fields are global event deltas during that collector's observation
window, starting at the GET transaction that supplied its first header/body.
They include interleaved packet traffic; they are correlated evidence, not proof
that each error belonged to that TID. The new-TID header can end the old window.
`padSlsHeaders`/`fullReplyOk` similarly include relevant observations during the
window, not per-segment delivery guarantees. Maxima are observed during the
window, not differences between lifetime maxima. An in-progress terminal drain
may finish after closure and thus not contribute to that closure's maxDrainMs.

A rejected CRC/length group was observed and refused. A zero bitmap bit is an
unobserved stored segment; the logs cannot assign it automatically to RF or the
chip. If holes persist with zero read/mismatch/CRC/rejection/physical/overflow
evidence, the missing data were not observed at the SLS parser/storage boundary.
An independent receiver or Si4684/on-air trace is then needed to distinguish
broadcast, RF and chip delivery behavior. Zero overflow alone never proves lossless
delivery.

## Hypotheses and evidence

| Hypothesis | Evidence/decision | Discriminating test |
|---|---|---|
| Extra 11A CTS is predominantly DSRV | Plausible, not historically decomposable | Frozen CMD vectors + RATE/nonDsrv, packet/PAD/empty/verify proportions. |
| Repeated Directory is repeated physical reply | Unproven; equal content/TID is insufficient | GET transaction count vs fullReads, verifyEmpty, irqObserved; vendor preserved-reply host test; independent over-air continuity evidence. |
| Packet service/FIFO/scheduling starves SLS | Unproven | B improves but C does not; compare queue histogram/latency and RF. |
| EPG parser CPU cost causes loss | Unproven | C improves similarly to B with packet draining unchanged. |
| UART/DEBUG causes loss | Plausible, not established for 11A | D improves; compare UARTdrop, host gaps, stack/heap. |
| All holes are overflow | Contradicted as a complete explanation | Early incomplete TIDs precede overflow; compare per-TID PATH deltas. |
| Silent full-read failures are invisible | Confirmed code gap; now counted exactly once | Production wrapper Timeout/Transport/Device and length mismatch tests. Not proof this caused historical holes. |
| JPEG decoder or buffer size is primary | No supporting evidence from these logs | Similar image sizes succeed on 11B; diagnose delivery before changing buffer/timeout. |
| Reported restarts are brownouts/watchdogs/panics | Unknown outside captured logs | Always-on readable ESP32 boot reason and captured panic trace; distinguish tuner recovery. |

## Hardware procedure (English)

Use the same firmware build, antenna, location, channel/service and settings.
Do not upload as part of this procedure without a separately authorized upload.
Keep a 115200 8-N-1 monitor with timestamps/log-to-file open across each reset.
Start **each variant from a fresh ESP32 boot**, select 11A PR Trójka and wait for
stable audio and packet ownership. Save the boot reason. For B, obtain normal
production packet reception first; if it never becomes Observing, B is unsupported.

1. Run `DSRV MODE A`, then `DSRV STATUS`. Require mode A, probe 4 and debug 1.
   Collect at least 20 complete/abandoned TIDs entirely inside that epoch.
2. From a fresh boot/stable production packet service, run `DSRV MODE B`.
   Require ACCEPTED, packet STOP result OK (or absent), then `DSRV STATUS`
   with mode B and probe 0. Confirm audio/DLS/PAD slideshow continue. Collect
   at least 20 TIDs. Run `DSRV MODE A`; verify packet START and probe 4 return.
3. From a fresh boot, run `DSRV MODE C`. Require mode C, probe 4. Confirm
   packet Start/GET/full reads continue, parseSkipped rises, PAD/audio continue.
   Collect 20 TIDs, then return A and verify normal parsing/reception resumes.
4. From a fresh boot, run `DSRV MODE D`. Require mode D, probe 4, debug 0.
   Do not send DEBUG or enable verbose. New RATE/CMD/QUEUE/PATH and SLS LOSS
   records still appear. Collect 20 TIDs, then return A and check debug 1.
5. Repeat on 11B PR Trójka as control, with the same build and procedure.

Ignore the first mixed interval and mixed-epoch TIDs. Expected records:
`[DSRV/MODE]`, then each 30 s `[DSRV/RATE]`, paced `[DSRV/CMD]`, `[DSRV/QUEUE]`,
`[DSRV/PATH]`; each closure `[SLS/LOSS]` + `[SLS/PATH]`. Keep UARTdrop/lossLogDrop
zero or report missing evidence. Record RSSI/CNR/FIC/lock, observed audio drops,
complete/abandoned TIDs, masks, reasons, per-TID error deltas, queue/wait/drain
maxima and heap/stack minima. Do not infer audible continuity from an audio flag.

If a reset occurs, retain `[BOOT] reset reason`, `rst`, Guru Meditation/backtrace
or brownout lines. `[DAB/WATCHDOG]`/`[DAB/STALL]` without a new ESP32 boot indicates
tuner recovery. Compare rates per actual `ms` and successful physical mode state.
B-only improvement suggests service/FIFO/scheduling; similar B/C improvement
suggests host parsing cost; D improvement suggests logging; no B improvement
keeps RF/broadcast/PAD hypotheses open. Improvement without reproducible baseline
is not a confirmed repair.

## Files and validation

Production: `src/dab_loss_diag.h` (fixed evidence banks/closed-TID queue),
`src/dab_loss_diag_output.h` (bounded formatter/pacing), `src/si4684.cpp/.h`
(precise hooks, full-read guard, A–D owner control), `src/dab_receive_health.h`
(shared diagnostic include), `src/comms.cpp` (commands), main `.ino` (reporter,
cached health and earlier boot/RF work). Documentation: this report,
`Serial_protocol.md`, existing English RF guide and `tests/README.md`.

Tests: `test_dab_loss_diag.cpp`, `test_dab_dsrv_header.cpp`,
`test_dab_receive_health.py`, `test_epg_transport.py`, `test_epg_coexistence.py`,
`test_epg_synthetic_paths.py`, `run_epg_tests.py`; offline analyzer above.
Mocks were updated to preserve the actual short header before full-read checks.

Final validation on 26 October 2026:

```powershell
$env:PYTHONDONTWRITEBYTECODE='1'
python tests/run_epg_tests.py --compiler C:/msys64/ucrt64/bin/g++.exe --input tests/fixtures/epg/device-monitor-261023-160924.log --reference tests/fixtures/epg/SI4684_MUX11B_EPG_kompletni_dekodovani.zip
& C:/Users/alesb/.platformio/penv/Scripts/pio.exe run -e esp32dev
git -c core.whitespace=cr-at-eol diff --check
```

| Check | Final result |
|---|---|
| Full EPG host runner | **PASS**, exit 0, all **35** compile/run steps, 46.79 s. Includes new diagnostics, actual collector faults, mode lifecycle, clock, scheduler, RAW, cache/runtime/UI boundaries, packet/PAD, synthetic paths/UART, storage, Adam7 and PC client. |
| ESP32 final build | **PASS**, exit 0, 7.11 s after final UART help change. Existing TFT_eSPI TOUCH_CS warning only. |
| Whitespace/diff check | **PASS**, exit 0. |
| Historical ZIP analyzer | **PASS**, exit 0; evidence limits above. |
| Real RF A–D, audio continuity, new HW heap/stack minima, actual reset reproduction | **NOT RUN / HW UNVERIFIED**. No port opened, no flash/NVS operation. |

| Target memory | Reference HEAD | Before this request (boot/RF work included) | Final | Change for this request |
|---|---:|---:|---:|---:|
| Static RAM | 67544 B | 67544 B | **71232 B** | **+3688 B** |
| Flash used | 3558373 B | 3559153 B | **3570153 B** | **+11000 B** |

Reference figures are the recorded successful preceding builds; final figures
come from the new target build. Final `.dram0.data=21800 B`, `.dram0.bss=49432 B`,
`.iram0.vectors=1027 B`, `.iram0.text=74747 B`. The linked diagnostic Evidence
symbol occupies **3664 B on ESP32** (host alignment gives 3688 B); the remaining
RAM increase includes the collector reason pointer and 30 s health cache.
The EPG arena remains 63600 B on target and there is still one SLS image buffer.
The fixed output formatter uses up to 1024 B of temporary stack; target stack
and heap minima must be measured in A–D. Static RAM growth reduces available
heap headroom; no mock minimum is presented as a new hardware measurement.

Complete local runner output: `.pio/epg_acceptance/results.log` and
`results.json` (ignored artifacts). The build generates firmware artifacts only;
it does not upload them. No regression was observed at tested host boundaries,
but host PASS is not an RF or audible-audio verification. No speculative P1
scheduling/FIFO workaround was applied.
