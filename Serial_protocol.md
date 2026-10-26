# Serial protocol

This document describes the serial protocol exactly as implemented by firmware
**v2.5b**. It is not a proposal for a future protocol. Current limitations and
implementation-specific behaviour are therefore documented explicitly.

The implementation is defined by `src/comms.cpp`, `src/epg_runtime.cpp`,
`src/epg_uart.cpp` and the radio routing in `src/si4684.cpp`.
Updated for the locally tested firmware on 24 October 2026.

## Transport

- Interface: ESP32 UART/USB serial console.
- Speed: 115,200 baud.
- Default Arduino Serial frame: 8 data bits, no parity, 1 stop bit.
- Commands and responses are line-oriented text.
- The firmware accepts `CR`, `LF`, and `CRLF` line endings.
- If a terminal sends no line ending, the firmware commits the command after
  approximately 100 ms without another received byte.
- The receive buffer holds 128 bytes including the terminating `NUL`. A longer
  command produces `#1`.
- Regular control and diagnostic command names are case-insensitive. Leading
  and trailing whitespace around the line is removed; regular `NAME=value`
  commands also trim the name and value. EPG commands use the spaces shown
  below; E1/E2 frame prefixes are uppercase and their CRC-covered text must
  not be normalized.
- Service names and text data are transmitted as UTF-8.
- Regular control/dynamic messages have no checksum or per-message delivery
  acknowledgement. EPG E1/E2 data frames have CRC32 and explicit ACK/NACK,
  independently of the embedded MSC CRC16.

## Output line types

| Prefix | Meaning |
|---|---|
| `*` | current setting or state acknowledgement |
| `:` | receiver capability information |
| `$` | dynamic runtime message |
| `#` | command result or error |
| `[` | diagnostic text outside the control protocol |
| `EPG ` | EPG command replies, ACK/NACK, status and object diagnostics |

Lines beginning with `[` may appear when diagnostics are enabled; the boot
reset-reason line is emitted even with diagnostics disabled. A structured
protocol monitor must ignore them.

## Opening and closing a connection

Structured serial output is disabled after startup. Enable it with:

```text
ENABLE=1
```

Disable it with:

```text
ENABLE=0
```

The enable response is sent in this order:

```text
*ENABLE=1,v2.5b,<chip type>/<chip firmware>
:MODE=3,3-3
*INTERVAL=<interval>
:FREQ=<count>,<index>:<kHz>,<channel>;...
*SERVICE=<index>                 only when a service is running
*TUNE=<DAB channel index>
$M=SLIDESHOW=0
```

The normal communication loop then publishes the current or changed `$L`,
`$I`, `$D`, `$S`, and, where applicable, `$M` messages.

Example:

```text
*ENABLE=1,v2.5b,SI4684/4.0.5
:MODE=3,3-3
*INTERVAL=100
:FREQ=38,0:174928,5A;1:176640,5B;...
*TUNE=11
$M=SLIDESHOW=0
```

`ENABLE=0` returns:

```text
*ENABLE=0
```

While the connection is disabled, `ENABLE` is the only regular protocol
command that is processed. Other commands containing `=` are silently ignored.
The service commands `DEBUG`, `DEBUG SLSV`/`DEBUG VERBOSE`, `DEBUG SPI`,
`DEBUG SPI STOP`, all `EPG TEST ...` / `EPG ARCHIVE ...` commands and E1/E2 fragments remain
available without an active connection. `ENABLE=0` does not stop an EPG test.

## Commands

### `ENABLE=<0|1>`

Enables or disables the structured protocol.

| Value | Result |
|---:|---|
| `0` | disable; response `*ENABLE=0` |
| `1` | enable and send the complete connection header |
| any other value | `#1` |

Sending `ENABLE=1` again while connected resends the connection header and
invalidates the dynamic output caches, causing the current state to be
published again.

### `INTERVAL=<1..500>`

Sets the `$S` message interval in milliseconds.

Command:

```text
INTERVAL=250
```

Response:

```text
*INTERVAL=250
#0
```

Firmware v2.5b does not accept zero; `INTERVAL=0` returns `#1`.

### `TUNE=<0..37>`

Tunes to a DAB Band III channel by its index in the advertised `:FREQ` table.

Command:

```text
TUNE=11
```

Response after accepting the request:

```text
#0
*TUNE=11
$M=SLIDESHOW=0
```

The command stops the current service, clears associated text data, and starts
asynchronous tuning. `#0` acknowledges that the request was accepted; it does
not mean that DAB lock was subsequently acquired.

The command returns `#1` while the receiver is in FM mode.

### `SERVICE=<index>`

Selects a service from the current `$L` list. The valid range is zero through
`COUNT - 1`.

Command:

```text
SERVICE=4
```

Response after accepting the request:

```text
#0
*SERVICE=4
$M=SLIDESHOW=0
```

Service startup is asynchronous. The command returns `#1` in FM mode or when
the index is outside the current service list.

## Diagnostic service commands

These commands do not contain `=` and are not part of the monitor data
interface.

### `DEBUG`

Toggles normal diagnostics. The response is:

```text
[DEBUG] diagnostics ON
```

or:

```text
[DEBUG] diagnostics OFF
```

Disabling normal diagnostics also disables verbose slideshow diagnostics.

### Restart and RF diagnostics (26 October 2026)

Every boot emits `[BOOT] reset reason=N (NAME)` even with DEBUG disabled.
Names distinguish POWERON, EXTERNAL, SOFTWARE, PANIC, INTERRUPT_WATCHDOG,
TASK_WATCHDOG, OTHER_WATCHDOG, DEEP_SLEEP_WAKE, BROWNOUT and SDIO; unrecognized
reasons use UNKNOWN. This is the ESP32 reset reason. A tuner cold recovery is
reported separately by the existing `[DAB/WATCHDOG]` / `[DAB/STALL]` diagnostics.
The reset category alone does not identify the exact faulty code or component.

With `DEBUG` ON, a best-effort `[DAB/RF]` or `[FM/RF]` line appears every 5 s
when at least 512 bytes of diagnostic queue space are available. `upMs` is
uptime in milliseconds (wraps after about 49.7 days). Values come from cached
status replies; logging performs no additional tuner transaction.

- `RSSIx10` is RSSI in tenths of dBuV (divide by 10); CNR/SNR are in dB.
- DAB `FIC` is quality in percent; `lock` is valid AND acquired. `sample` is
  the RF sample generation: an unchanged value means no new parsed RF reply.
- DAB `requestedKhz` is the requested channel, `tune` indicates pending tuning.
  Cached metrics during tuning must not be attributed to the new channel.
- DAB `SLS` is slideshow signalling, `completedTID` the last completed MOT transport ID (zero if unknown),
  `bytes=received/expected` the collector progress and `ready` image availability.
  An unknown expected size is zero; completedTID does not identify the partial object.
- DAB `heap`/`minHeap` are current/minimum free heap bytes. Compare progress
  stalls with lock/FIC/CNR, existing CTS/overflow counters and `[SERIAL] dropped`.
- FM `khz`, `valid`, `SNR` and `multipath` describe the cached FM status;
  multipath is the filtered percentage used by the UI.

Enable `DEBUG` before collecting the failure log and keep the monitor open
across a restart to capture the boot reason and any ESP32 panic/backtrace.
`DEBUG SLSV` is not required for these summaries.

### `DSRV MODE A|B|C|D` and `DSRV STATUS`

Temporary RAM-only loss-diagnostics experiment, available without ENABLE.
A is normal packet parsing with DEBUG on; B requests STOP of the separately
owned packet SPI service while keeping audio/PAD/SLS; C drains packet replies
but omits packet EPG Core parsing; D is normal reception with ordinary DEBUG
and verbose off, retaining bounded aggregates. No setting is written to NVS.

`[DSRV/MODE] requested=B result=ACCEPTED probe=5` acknowledges a request, not
completed hardware STOP. Use `DSRV STATUS` to verify mode B/probe 0 (Idle), then
mode A/probe 4 (Observing) after return. Shared/missing packet ownership, FM,
replay or cleanup failure can return UNSUPPORTED. In-flight START/STOP uses the
existing owner cleanup sequence. A/C/D restore automatic production acquisition.

`[DSRV/RATE]`, `[DSRV/CMD]`, `[DSRV/QUEUE]` and `[DSRV/PATH]` report frozen
30 s interval and cumulative evidence, paced at most once per 100 ms with UART
headroom. `[SLS/LOSS]` and `[SLS/PATH]` describe completed/abandoned collectors,
including a full missing-segment bitmap. Missing LAST means a lower bound, not
an invented total. Counters saturate; lost diagnostic records are counted.
`mixed=1` marks a rate interval crossing mode changes. Use a fresh boot for each
variant and at least 20 closed TIDs; new hardware measurements remain required.

See [field definitions, evidence and complete A/B hardware procedure](docs/SI4684_DSRV_SLS_DIAGNOSTICS_2026-10-26.md).

### `DEBUG SLSV` or `DEBUG VERBOSE`

Enables normal diagnostics and toggles detailed MOT segment and JPEG scan
diagnostics. The response is:

```text
[DEBUG] SLS per-segment verbose ON
```

or the corresponding `OFF` message.

Diagnostics should normally be disabled while a machine-readable monitor is
connected because diagnostic and protocol lines share the same UART.

### DAB component discovery diagnostics

Enabling `DEBUG` queues the current inventory and any completed summary; it
does not restart or delay the UA discovery pass. `[DAB/SVC]` reports each
SID, P/D, component count and list version; `[DAB/COMP]` reports each exact
32-bit CID, TMID, ASCTy/DSCTy, DG, P/S, CA and UA-valid flags. Packet-mode
MOT additionally produces an `identity=unverified` candidate line. Neither
MOT transport nor UA-valid=0 proves the presence or absence of SPI.
Identical periodic service lists do not repeat the inventory dump; changed
version, P/D or CID topology invalidates the pass.

The independently paced summary reports the published result, for example:

```text
[DAB/INV] services=10 components=10 audio=10 data=0 packetMOT=0 invalid=0 version=7
[DAB/UA] scan complete services=10 components=10 processed=10 failed=0
[DAB/SPI] confirmed=0 packetMOTUnverified=0 status=not signalled selected=0 SID=00000000 CID=00000000 profile=unknown
```

`audio` and `data` count programme/data **services by P/D**, not component
transport modes. `processed` counts successfully parsed UA responses; `failed`
counts failed commands/invalid responses. A completed pass satisfies
`components == processed + failed`. Any failure yields `discovery incomplete`.
`not signalled` means SPI was not found in the available signalling, after a
complete error-free inventory/UA scan with no unverified packet-MOT component.
A malformed service list retains the last good station table, reports
`invalid=1` and `scan incomplete`, and blocks endpoint selection until a new
valid list arrives. Counts in that invalid-inventory report refer to the retained
last valid list; confirmed presence can also retain last valid evidence.

`confirmed` counts validated 0x007 advertisements. `packetMOTUnverified` counts
TMID=3/DSCTy=60 components without confirmed SPI identity. `selected=1` requires
an unambiguous endpoint and complete successful discovery. Confirmed presence
can therefore coexist with `selected=0` (ambiguous or incomplete). The final
asynchronous reply must be committed before publication. UART output is
independent: blocked logging never delays queries, presence or SLS evaluation.

Discovery runs quietly with DEBUG disabled. Diagnostic lines share 100 ms pacing
and a 256-byte UART headroom check. Detailed UA dumps use one best-effort slot;
while it is pending, newer diagnostic copies are skipped, but every UA answer
is processed. Retuning/reconfiguration discards stale dumps and summaries. Detection repeats throughout the DAB session:
60 seconds after each completed UA pass, including failed UA passes, another
low-priority pass is scheduled. Loss of lock, switching or a tracked SPI probe
postpones it until eligible. Invalid service-list structure requires a new valid
service list instead of a UA-only retry. Retuning, changed list/version and audio
selection invalidate in-flight UA ownership. Routine refreshes retain the last
confirmed presence and do not repeat an unchanged inventory dump.

The small `EPG` indicator below the codec now uses four static UI states:

| Indicator | RGB565 | Meaning |
|---|---|---|
| Hidden | — | No confirmed SPI for the current context, or FM. |
| Red | 0xF800 | Confirmed SPI, waiting for relevant data within the UI progress window. |
| Cyan | 0x06DF | Usable current-service PI or decoded, unambiguous current-service SI. |
| Muted grey | 0x3A4A | Empty Directory, unusable/ambiguous data, stale PI without usable SI, or a 120-second wait without progress. |

Green is no longer used for this text. Codec colours/alternation, slideshow
colours, font, coordinates and the rest of the layout are unchanged. Colour
changes are not animated. The previous text is removed when the indicator hides.

Cyan does not promise current NOW/NEXT: valid SI remains useful without PI,
with historical PI, or while the broadcast clock is uncertain. Historical PI
alone cannot establish current programme availability. A logo alone is not
sufficient. Cached SI/PI must match the current audio/ensemble and active source;
a newly selected source cannot borrow the previous source's cyan status.
A routine incomplete UA refresh of an unchanged confirmed endpoint does not
invalidate useful cached SI. The existing schedule/clock rules for READY_LIVE
in STATUS and the EPG pages are unchanged.

The compile-time UI limit is `epg::kIndicatorWaitMs = 120000`. Its monotonic
clock starts with a new source/service/test context. Only new owned valid
Directory content, new valid fragments, or a newly decoded object extend it.
The first incomplete Directory also counts newly received segment bits; once
known, identical repeats do not invent new content. Matching duplicates,
invalid groups, clock updates, redraws and unowned packet/PAD traffic do not
extend this window. Expiry changes only the indicator to grey: it never clears
cache, fragments, TIDs, priority, or a physical data service. New real progress
can restore red, and usable SI/PI takes precedence over the UI timeout.
In replay, the validated Directory or explicitly confirmed synthetic INJECT
source supplies presence; the physical radio service does not select the test
station. FM still hides the main-screen indicator.

For details and reproducible tests see
[the packet-component report](tests/DAB_SPI_packet_components_report.md).

For P/S verification, `CID=01FE0002` must report `PS=primary` (ETSI EN 300 401
section 6.3.1). CID and all other classification fields remain unchanged.
A corresponding synthetic `CID=01FC0002` fixture reports `PS=secondary`.

`DAB_XPAD_ENABLE` remains 0x0097. During DAB initialization only, a successful
SET_PROPERTY is followed by one GET_PROPERTY (250 ms timeout) using the existing
boot command API. DEBUG prints the cached result, not a new radio transaction:

```text
[DAB/XPAD] requested=0x0097 setResult=0 readback=0x0097 getResult=0 match=1 fw=6.0.9 rev=unverified
```

`setResult` and `getResult` are separate vendor Result codes; 0 means Ok, -4
means timeout. `setResult=127` means no attempt was recorded. A failed write
skips the read (`getResult=not-attempted`). Failed or absent reads print
`readback=unavailable` without `match`. Successful unequal reads print `match=0`.
Boot/recovery/hardware reset invalidates the cached result; FM never reads B400.
DEBUG toggles may repeat the cached diagnostic line, never the GET_PROPERTY.

`[DSRV/UA]` preserves the raw field and adds `meaning`, source name, routing
context, scan generation and suppressed-sample count. Raw 2/7 at source 0/1
prints SLS-candidate/SPI-candidate; source 2 prints DLS-raw regardless of raw UA,
and source 3 prints reserved. `context` reports audio/sls/spi/unmatched/inactive
using existing endpoint and audio-state rules. Raw interpretation does not
supply endpoint confirmation. The X-PAD EPG adapter additionally requires a
validated current-component SPI UA answer and matching audio SID/CID before
using UATYPE=7 to select the EPG decoder. Other sources do not use raw UA as
an application discriminator.

One line globally per five seconds within a scan generation is allowed, only
with DEBUG and at least 256 bytes of UART headroom. Suppressed counts saturate;
retune/new generation discards the old diagnostic window. No payload is retained
for later logging. `rev=unverified` remains because the firmware UATYPE contract
has not been established. See [the diagnostics report](tests/DAB_XPAD_UATYPE_diagnostics_report.md).

### `DEBUG SPI`

Enables diagnostics and requests one controlled SPI coexistence probe. Requires
stable DAB audio, a complete unambiguous SPI discovery result, an idle probe,
and SPI SID/CID distinct from both audio and the active standalone SLS context.
The packet coexistence probe still refuses a shared audio endpoint. X-PAD EPG
is admitted independently on the playing audio component with confirmed SPI UA
and DATA_SRC=1/UATYPE=7; it does not issue a second START for the audio endpoint.
If discovery has not finished, wait for its result and send the command again;
the request is not silently queued for a later station.

The probe records 5 seconds of baseline DSRV traffic, then schedules a
low-priority START for SPI **without STOP for audio or SLS**. It observes traffic
for 30 seconds after successful START and schedules STOP for the probe's own
SPI SID/CID. SPI payload is counted and discarded, never passed to the SLS
collector. While the probe tracks a SPI context, audio/SLS/PAD packets use
the existing decoder only after matching SID/CID and DATA_SRC; other contexts
are drained/ignored. Outside the probe, the original SLS/DLS path is restored,
including its existing audio-switch and collector stale-MOT protections. An
unmatched diagnostic SID/CID alone does not discard normal SLS/DLS reception.
Failed probe cleanup retains strict routing until retune/reset.

With DEBUG enabled, `[DSRV/IDENTITY]` reports unmatched received SID/CID/source
alongside expected audio/SLS identities and validity flags, even without SPI.
`legacy=1` means the payload is forwarded to the original decoder (which still
applies its own checks); `legacy=0` means it is withheld. Output is limited to
one queued line per 5 seconds with a suppression count and UART-space check.
Hardware captured audio SID `00002FD9` with service-list CID `01FE002A`, while
both PAD (`src=1`) and DLS (`src=2`) report CID `0000002A`. For these two sources
only, an audio TMId=0 endpoint accepts its zero-extended low 16-bit component
field with the same SID. Full CID matches remain accepted. Nonzero mismatching
upper halves, different SID/component fields and unknown sources are rejected
by the router. Standalone SLS/SPI identities still require exact matching;
their short-CID representation has not been verified. START/STOP and other
component commands continue using the full original service-list CID.

`[DSRV/ROUTE]` output contains route, source, SID/CID, DSCTy, byte count, remaining
buffers and service state. Route values: 0 ignored, 1 audio/PAD, 2 standalone
SLS path, 3 SPI. Each route logs at most twice a second; suppressed lines are
counted. Summaries every 5 seconds compare baseline and observation packet
counts, report tuner SLS STOP notifications and CTS/overflow/UART-drop deltas.
Distinct packet traffic alone does not prove uninterrupted audio or decoded
slideshows. Missing packets are inconclusive because of carousel timing.

Changing station/multiplex, changing the component inventory or disabling DEBUG
cancels the probe. A START still awaiting CTS is reconciled before cleanup,
even after a new tune generation. Probe STOP takes priority over normal service
teardown; up to three attempts use cooperative 500 ms retry gaps, with DSRV
draining continuing between attempts. A NOT_AVAILABLE STOP response counts as
already stopped. An unanswered START also triggers conservative STOP.

If cleanup fails, the log says `cleanup uncertain`; the possible SPI context
remains quarantined and another probe is refused until a retune/reset. Normal
CTS recovery rules still apply. This probe does not restore a standalone SLS
service replaced by firmware: note the evidence and retune/reselect the station
after testing to restore normal service selection. No time-sharing fallback is
enabled and simultaneous standalone SLS/SPI remains unverified on FW 6.0.9.

### `DEBUG SPI STOP`

Requests early cancellation and cleanup of the current packet SPI probe and
pauses automatic packet SPI until the next tune. It does not stop audio or
the SLS component. Like DEBUG SPI, it is available without ENABLE=1.
Use `EPG TEST STOP` to leave UART TEST mode; it is a different command.

## Result codes

| Code | Current meaning |
|---|---|
| `#0` | an `INTERVAL`, `TUNE`, or `SERVICE` command was accepted |
| `#1` | value outside the accepted range, or receive-line overflow |
| `#2` | unknown command or command without the expected syntax |

`ENABLE` uses its own state response and does not emit `#0`.

### Current numeric parser behaviour

Firmware v2.5b uses `strtol()` without validating the complete input string.
A monitor must account for these properties of the current implementation:

- a non-numeric value is converted to zero;
- a numeric prefix followed by other characters is accepted, for example
  `TUNE=12abc` is interpreted as `12`;
- `TUNE=abc` can be accepted as `TUNE=0`;
- `SERVICE=abc` can select service 0 if that service exists;
- `ENABLE=abc` is interpreted as `ENABLE=0`.

The monitor must therefore validate every integer before transmitting it.

## State and dynamic messages

### Current channel

```text
*TUNE=<index>
```

Sent during connection setup and when the DAB channel is changed through the
protocol or local user interface.

### Current service

```text
*SERVICE=<index>
```

Sent during connection setup if a service is running, and when the running
service subsequently changes.

### Service list `$L`

```text
$L=COUNT=<count>,ENSEMBLE=<EID>,<name>;SERVICES=<index>,<type>,<name>;...
```

Example:

```text
$L=COUNT=3,ENSEMBLE=1234,Example DAB;SERVICES=0,4,Radio One;1,4,Radio Two;2,3,Data
```

The list is sent on the first communication pass after connection and then
only when monitored list data changes. At most 32 services are transmitted. If
the receiver has no signal lock, the services section is:

```text
SERVICES=0
```

Service types used by the firmware:

| Type | Meaning |
|---:|---|
| 0 | AUDIO STREAM SERVICE |
| 1 | DATA STREAM SERVICE |
| 2 | FIDC SERVICE |
| 3 | MSC DATA PACKET SERVICE |
| 4 | DAB+ |
| 5 | DAB |
| 6 | FIC SERVICE |
| 7 | XPAD DATA |
| 8 | NO MEDIA |

The `,`, `;`, and `=` delimiters are not escaped in names. A monitor must not
treat the complete line as unrestricted CSV data.

### Service information `$I`

```text
$I=ID=<component>;SID=<SID>;PTY=<pty>;PROTECTION=<protection>;SAMPLERATE=<Hz>;BITRATE=<kbps>;AUDIO=<mode>
```

If no service is running:

```text
$I=ID=0;SID=0;PTY=0;PROTECTION=0;SAMPLERATE=0;BITRATE=0;AUDIO=0
```

`ID` is the low byte of the selected service component ID. `$I` is sent after
connection and subsequently only when monitored values change.

Protection values:

| Value | Meaning |
|---:|---|
| 1–5 | UEP 1–5 |
| 6–9 | EEP A-1 through A-4 |
| 10–13 | EEP B-1 through B-4 |

Audio modes:

| Value | Meaning |
|---:|---|
| 0 | dual |
| 1 | mono |
| 2 | stereo |
| 3 | joint stereo |

### Dynamic Label/radiotext `$D`

```text
$D=RT=<UTF-8 text>
```

The current text is sent once after connection and again whenever it changes.
An empty text is valid:

```text
$D=RT=
```

The text is not escaped.

### Signal `$S`

```text
$S=SIGNAL=<decimal value>,LOCK=<0|1>,CNR=<value>,FIC=<0..100>
```

Example:

```text
$S=SIGNAL=43.9,LOCK=1,CNR=18,FIC=100
```

The message is periodic according to `INTERVAL`. `SIGNAL` is the internal
display signal level divided by ten, `LOCK` reports DAB synchronization, `CNR`
is the carrier-to-noise ratio, and `FIC` is FIC quality.

### Slideshow `$M`

```text
$M=SLIDESHOW=<type>
```

| Type | Meaning |
|---:|---|
| 0 | no slideshow is available, or it was invalidated by a channel/service change |
| 1 | a JPEG object is ready |
| 2 | a PNG object is ready |
| 3 | the object has an unknown or unsupported signature |

Firmware v2.5b does not transmit image bytes or Base64 data. This message only
announces the type of the locally received MOT object.

`$M=SLIDESHOW=0` is always sent during `ENABLE=1`. If no slideshow is available,
the current implementation may send the same line once more immediately. If
an image is available, its type is sent by the following normal communication
pass. A monitor must tolerate duplicate messages.

## DAB frequency table

The complete table is sent in the `:FREQ` line. Firmware v2.5b uses this
index mapping:

```text
0=174928/5A   1=176640/5B   2=178352/5C   3=180064/5D
4=181936/6A   5=183648/6B   6=185360/6C   7=187072/6D
8=188928/7A   9=190640/7B  10=192352/7C  11=194064/7D
12=195936/8A 13=197648/8B  14=199360/8C  15=201072/8D
16=202928/9A 17=204640/9B  18=206352/9C  19=208064/9D
20=209936/10A 21=211648/10B 22=213360/10C 23=215072/10D
24=216928/11A 25=218640/11B 26=220352/11C 27=222064/11D
28=223936/12A 29=225648/12B 30=227360/12C 31=229072/12D
32=230784/13A 33=232496/13B 34=234208/13C 35=235776/13D
36=237488/13E 37=239200/13F
```

Frequencies are expressed in kHz.

## FM limitations of the current protocol

Firmware v2.5b supports FM through its local user interface, but its serial
interface remains the original DAB protocol:

- `:MODE` always reports `3,3-3`;
- `:FREQ` always contains the DAB table;
- `TUNE` and `SERVICE` return `#1` in FM mode;
- no command is defined for FM frequency, region, or seek;
- no separate PI, PS, RDS/RBDS, stereo, blend, multipath, or AFC-rail messages
  are defined;
- periodic `$S` retains the original DAB-shaped format.

The receiver should be in DAB mode when controlled through this protocol.

## Monitor implementation recommendations

- Send `ENABLE=1` after opening the port and wait for `*ENABLE=1`.
- Do not derive a protocol version from application version `v2.5b`; this
  firmware does not transmit a separate protocol version.
- Parse complete lines independently and tolerate asynchronous ordering of
  dynamic messages.
- Tolerate repeated state messages, particularly `$M=SLIDESHOW=0`.
- Ignore unknown capability lines beginning with `:` and diagnostic lines
  beginning with `[`. Dispatch `EPG ` lines to the EPG reply parser; they may
  be interleaved with regular monitor messages.
- Do not assume that `#0` means signal lock or completed service startup.
- Validate command values in the monitor before transmitting them.
- Do not expect Base64 image data after an `$M` message; this firmware does not
  transmit it.



## EPG live reception and UART test modes

The packet EPG service starts automatically after validated SPI discovery and
stable DAB audio. Confirmed audio-component X-PAD SPI can feed EPG through
DATA_SRC=1/UATYPE=7. These inputs share one bounded MOT/SPI Core; EPG bytes do
not enter the slideshow decoder. Confirmed PAD takes priority over packet
input. Changing transport/component clears incomplete objects and cached
programmes to prevent colliding MOT transport IDs from mixing.

`DEBUG SPI` is the separate 5 s baseline / 30 s packet coexistence probe,
not an EPG replay command. `DEBUG SPI STOP` pauses automatic packet SPI until
the next tune; it does not select LIVE versus TEST.

### Switching between LIVE and TEST

LIVE uses broadcast identities and time. TEST uses an isolated replay,
synthetic DSRV input or built-in demo, marked `TEST` in the EPG UI. Starting a
test clears the shared EPG cache and suppresses live EPG feeding. Audio tuning
and NVS settings are not changed by EPG test commands. Live and test EPG do
not decode concurrently. Selecting a virtual station does not retune the radio.

To inspect a live receiver:

```text
EPG TEST STATUS
```

To start the built-in programme demonstration:

```text
EPG TEST DEMO
EPG TEST STATUS
```

To return to real reception:

```text
EPG TEST STOP
EPG TEST STATUS
```

Stop the PC sender loop first, using its own console/exit mechanism. A second
monitor cannot open COM4 while the sender owns it. STOP closes the test UI,
clears virtual metadata/time, partial UART frames and EPG cache, and returns
to LIVE; programmes must then be received again. It does not require a reboot
and does not release the reserved EPG memory. CLEAR currently has the same
behaviour, including clearing a live cache if issued in LIVE.

A radio service/multiplex or FM/DAB change cancels the test. Automatic seeking
under weak signal can therefore terminate it. Wait for initial tuning to settle
before starting replay. Cancellation messages are:

```text
EPG INFO replay cancelled by radio service/mux change
EPG INFO replay idle timeout; returning to live
```

A UART replay/injection session times out after 30,000 ms without activity.
Send `EPG TEST STATUS` as a heartbeat when browsing after data transmission.
Built-in DEMO is exempt from that inactivity watchdog. An incomplete group
expires after 5,000 ms without an accepted fragment; restart the session after
this loss of assembly state. Neither TEST nor ENABLE is persisted across reset.

### Complete EPG command reference

All commands below work without `ENABLE=1` and without enabling DEBUG. The
`EPG TEST` spelling and command keywords are case-insensitive, but ISO time
requires uppercase `T`/`Z`. Replies use `EPG ...`, not `#0`/`#1`.

| Command | Behaviour and reply |
|---|---|
| `EPG TEST HELP` | Lists supported commands: `EPG OK HELP: ...`. Does not allocate Core. |
| `EPG TEST STATUS` | Reports LIVE/TEST and all diagnostics below. Allocates Core if needed; can return a memory error. |
| `EPG TEST TRANSPORT` | Queues paced, separate PACKET/PAD diagnostics, identity, reset reasons and health. Works without Core allocation; see transport diagnostics below. |
| `EPG TEST DEMO` | Starts the built-in historical MSC recording, opens EPG and replies `EPG OK DEMO source=actual-MUX11B MSC directory+F64C+F629; TEST historical data`. |
| `EPG TEST START` | Starts an empty E1 session for packet identity E23153FE/01F2C00C and opens EPG. |
| `EPG TEST START SID FULLCID` | As above with explicit hexadecimal input identity; the CID must have packet-mode bits `(CID & 0xC000) == 0xC000`. |
| `EPG TEST INJECT MSC` | Starts E2 synthetic DSRV injection with packet input. |
| `EPG TEST INJECT PAD` | Starts E2 synthetic DSRV injection with simulated confirmed SPI UA. |
| `EPG TEST INJECT PAD NOSPI` | Starts PAD injection without confirmed UA, for rejection tests. |
| `EPG TEST TIME ISO8601` | Sets test UTC/timezone for programme selection; replies `EPG OK TIME`. Requires TEST. |
| `EPG TEST STATION HEX` | Selects a virtual bearer or unambiguous SID; replies `EPG OK STATION` or `EPG OK STATION pending SI`. Requires TEST. |
| `EPG TEST STOP` | Cancels TEST, clears EPG state and returns to LIVE; replies `EPG OK STOP`. Also works in LIVE. |
| `EPG TEST CLEAR` | Currently the same reset as STOP; replies `EPG OK CLEAR`. |

Every START, INJECT or DEMO begins with an empty EPG cache. START/INJECT create
a new session token. START defaults to the historical Trójka selection
`40e231533233` at 2024-11-12T03:30:00Z with display offset +01:00; clients should
explicitly select their desired time and station. DEMO instead selects
Jedynka `40e231533211` at that time and contains the historical directory/SI/PI,
not a synthetic schedule or a PNG logo.

START reply:

```text
EPG OK START V1 session=HHHH fragment=32 group=4071 timeout=5000 idle=30000
```

Time examples (seconds and explicit timezone are mandatory):

```text
EPG TEST TIME 2024-11-12T04:30:00+01:00
EPG TEST TIME 2024-11-12T03:30:00Z
```

The parser accepts exactly `YYYY-MM-DDTHH:MM:SSZ` or
`YYYY-MM-DDTHH:MM:SS+HH:MM` / `-HH:MM`, validates the calendar and limits the
offset to ±14:00. TIME changes only the test clock, not the receiver's broadcast
clock or NVS. It does not replace the historical date with today's date.

STATION examples (hexadecimal without `0x`):

```text
EPG TEST STATION 40e231533211
EPG TEST STATION 3211
```

Up to 16 hex digits are accepted. Values at most FFFFFFFF are interpreted as
SID; larger values must be a valid full bearer. A SID selected before SI arrives
is held pending mapping. Missing/ambiguous mapping after SI is reported as an
error. Selecting a station does not itself supply its programme or logo.

### E1 and E2 acknowledged data frames

E1 is available after START and carries a complete CRC-bearing MSC data group,
without the 24-byte DSRV reply header. E2 is available after INJECT and carries
the whole DSRV reply including that header. DEMO accepts neither. Frame prefixes
are uppercase and must match the session mode.

```text
E1 SESSION SEQ OFFSET TOTAL SID CID SOURCE HEX CRC32
E2 SESSION SEQ OFFSET TOTAL SID CID SOURCE HEX CRC32
```

| Field | Recommended hexadecimal width | Meaning |
|---|---:|---|
| SESSION | 4 | Token returned by START/INJECT; use the new token after every restart. |
| SEQ | 8 | Fragment sequence, starting at 00000001; increments across group boundaries. |
| OFFSET | 4 | Byte offset into this group/reply; begins at 0000. |
| TOTAL | 4 | Total bytes in this complete MSC group (E1) or DSRV reply (E2). |
| SID | 8 | Outer session service identity. |
| CID | 8 | Outer session component identity. |
| SOURCE | 1 | 0 for E1 and E2 MSC; 1 for E2 PAD. |
| HEX | 2–64 | 1–32 payload bytes, encoded as an even number of hex digits. |
| CRC32 | 8 | CRC32 of the exact ASCII prefix from E1/E2 through HEX. |

Use single ASCII spaces and the indicated zero-padded widths. With 32 payload
bytes, a frame occupies 120 ASCII bytes before LF, 121 including LF. The CRC
excludes the space before CRC32, CRC32 itself, and CR/LF. It is standard
reflected CRC32 (polynomial 0xEDB88320, initial/final XOR 0xFFFFFFFF), compatible
with Python `binascii.crc32(body.encode('ascii')) & 0xFFFFFFFF`. Upper/lowercase
hex is accepted, but changing case changes this ASCII CRC. The embedded MSC
CRC16 is separate and must also be valid.

E1 TOTAL is at most 4071 bytes. E2 TOTAL also is at most 4071, leaving at most
4047 bytes for the DSRV payload after its 24-byte header. No partial, oversize
or interleaved groups may be committed. TOTAL remains constant within a group;
OFFSET follows the previously accepted bytes. The next group starts OFFSET=0
while SEQ continues. SID/CID must match the session endpoint; supported packet
short-CID matching is accepted for source=0, and audio short-CID matching for
PAD. A raw UATYPE value alone does not establish component ownership.

Send one fragment at a time and wait for:

```text
EPG ACK HHHH SSSSSSSS
```

ACK means that fragment was accepted; the final fragment also means the complete
group was admitted. It does not promise that asynchronous decoding has finished
or that a programme/logo is ready; check STATUS. An exact retransmission of the
last accepted sequence/CRC is ACKed without feeding the group again. Do not
advance SEQ on timeout or NACK BUSY. On a lost response, resend the identical
line; after a short delay retry BUSY without changing sequence/offset/content.

The command loop handles at most one line per pass. EPG text commands require
1024 bytes of diagnostic UART headroom and frames require 160; insufficient
headroom can leave a command unanswered without committing it. Clients must use
bounded retries and allow asynchronous monitor/diagnostic lines between replies.
Prefer the supplied stop-and-wait clients over pasting a large stream into a
terminal. Outer line-buffer overflow still produces the general `#1` result.

### EPG errors and recovery

| Reply | Meaning / recovery |
|---|---|
| `EPG NACK BUSY` | Core is still processing; delay and retry the same fragment. |
| `EPG NACK CRC` | ASCII frame CRC32 mismatch; correct the frame before resending. |
| `EPG NACK FORMAT` | Wrong E1/E2 mode or malformed fields, lengths or hex. |
| `EPG NACK ORDER` | Wrong session, sequence, offset, TOTAL or changed retransmission. Restart if assembly state was lost. |
| `EPG NACK IDENTITY` | Wrong outer SID/CID/source for this session. |
| `EPG NACK DATA` | Completed group/reply rejected, e.g. bad MSC CRC, malformed DSRV or failed admission rules. Start a new session before resending corrected data. |
| `EPG ERROR START required` | No suitable TEST session; also returned for E1/E2 during DEMO and TIME/STATION in LIVE. |
| `EPG ERROR START identity` | Invalid explicit START SID/CID syntax or non-packet CID. |
| `EPG ERROR TIME use ISO8601 with Z or explicit offset` | Invalid time/date/timezone. |
| `EPG ERROR STATION hex expected` | Empty/non-hex/overlong virtual identity. |
| `EPG ERROR STATION invalid bearer` | Full bearer encoding is invalid. |
| `EPG ERROR STATION unknown or ambiguous; use full bearer` | SID cannot be uniquely selected from SI. |
| `EPG ERROR STATION unknown or ambiguous after SI` | Pending SID could not be resolved when SI arrived. |
| `EPG ERROR unknown command; EPG TEST HELP` | Unsupported EPG subcommand in an active test. In LIVE the state check can instead return START required. |
| `EPG ERROR MEMORY need8=... free8=... largest8=... need32=... free32=... largest32=...` | Core allocation refused; inspect both memory pools and contiguous block sizes. |

EPG errors do not use the regular monitor's `#` code set. A memory error may
precede command-specific validation because Core is allocated on demand.

### STATUS and asynchronous object diagnostics

STATUS has no final end marker. It emits the original six lines followed by
`EPG STATE` and `EPG CLOCK`, plus `EPG INJECT` as the last line during injection:

```text
EPG OK STATUS mode=LIVE|TEST frames=N crcBad=N invalid=N dup=N conflict=N incomplete=N busy=N
EPG DATA dirs=N objects=N PI=N SI=N PNG=N programmes=N cached=N limited=0|1 ambiguous=0|1 now=N next=N
EPG RX unmapped=N oversize=N timeout=N fresh=0|1 continuityJumps=N unsupported=N
EPG RAM arena=N free=N min=N largest=N audio=0|1 SLS=0|1 UARTdrop=N rxCRC=N rxOrder=N rxIdentity=N
EPG LOGO bytes=N interlace=0|1
EPG POOLS free8=N largest8=N free32=N largest32=N
EPG STATE state=NAME transportFresh=0|1 scheduleDateValid=0|1 serviceHasPI=0|1 serviceHasSI=0|1 clockValid=0|1 sourceStart=N sourceEnd=N archive=0|1
EPG CLOCK encoding=0|1|2 valid=0|1 offsetKnown=0|1 offset=N utc=N local=N generation=N age=N
EPG INJECT path=MSC|PAD ua=0|1 accepted=N legacy=N rejected=N blocked=N
```

The alternatives above describe the grammar, not literal transmitted `|` values.
Counters are decimal; `now`/`next` are zero-based indices into the current view,
or -1 if unavailable. Fields mean:

| Line | Interpretation |
|---|---|
| `EPG OK STATUS` | Active mode and MSC group counters: input, CRC failures, malformed data, duplicates, conflicting segments, incomplete assemblies and busy feeds. |
| `EPG DATA` | Completed directory/object/PI/SI/PNG counts; decoded programme counter versus `cached` items for the selected station/day; capacity/ambiguity flags and NOW/NEXT indices. |
| `EPG RX` | Unmapped input, oversize objects, assembly timeouts, selected cache freshness, continuity-index jumps and unsupported content. A continuity jump alone is not a CRC failure. |
| `EPG RAM` | Arena bytes and free/minimum/largest heap block; audio service-start and slideshow-available flags, diagnostic UART dropped bytes and E1/E2 CRC/order/identity error counters. Audio=1 does not prove audible uninterrupted playback. |
| `EPG LOGO` | Selected cached PNG size and IHDR interlace flag; bytes=0 means no cached logo, regardless of the interlace=0 placeholder. |
| `EPG POOLS` | Free/largest blocks for INTERNAL\|8BIT DRAM and INTERNAL\|32BIT\|EXEC IRAM. Aggregate free heap alone does not establish allocatability. |
| `EPG INJECT` | Simulated UA confirmation and accepted, legacy-handoff, rejected and packet-blocked group counts for the injection session. |

The current Core arena for ESP32 is 63,600 bytes (target compiler layout check;
current RF firmware has not been hardware-tested). This includes two 12 KiB RX
blocks, the existing 12 KiB cached logo, 4 KiB Directory and 8 KiB text cache.
The preceding single-RX-block version used 51,096 bytes. Capacity is shared across
station/day cache contexts; the decoder intentionally does not retain every
programme/logo in a multiplex. `cached`, `fresh` and readiness apply to the
selected context. They are not proof of complete programme coverage.

Completed objects can emit this best-effort line even without DEBUG:

```text
EPG MOT tid=HHHH kind=DIR|PI|SI|PNG bytes=N programmes=N cached=N
```

It is not an ACK or a dump of programme/image bytes. `tid` is hexadecimal;
statistics and lengths are decimal. Low UART headroom may suppress this log
without preventing decoding.

### Synthetic DSRV injection details and PC clients

INJECT replies:

```text
EPG OK INJECT V2 session=HHHH path=MSC fragment=32 group=4071
EPG OK INJECT V2 session=HHHH path=PAD fragment=32 group=4071
```

Outer E2 identities are fixed per mode:

| Mode | SID | Full CID | SOURCE | UA confirmation |
|---|---|---|---:|---|
| MSC | E23153FE | 01F2C00C | 0 | Not needed for the synthetic packet endpoint |
| PAD | 00003201 | 01FE0005 | 1 | Simulated confirmed SPI |
| PAD NOSPI | 00003201 | 01FE0005 | 1 | Deliberately absent |

The inner header uses the real Si468x 24-byte DSRV layout and declares the exact
payload byte count. It can deliberately carry different SID/CID/source to test
rejection. PAD admission requires DATA_SRC=1, UATYPE=7, DSCTy 0 or 60, matching
audio identity and simulated confirmation; valid service states are 0 or 3
without physical-error flags. Inner packet input is admitted in MSC mode and
counted as `blocked` without feeding Core in PAD mode. Valid SLS/DLS audio
handoff increments `legacy` only; injection does not mutate the real slideshow
collector or decode a synthetic DLS message.

This mode exercises the actual header parser, shared admission rules and Core
on the MCU. UA/endpoint context is simulated: it does not establish RF reception,
FIC discovery, chip PAD extraction or simultaneous physical FIFO behaviour.

RAW E1 replay (the client owns the port and has its own command console):

```powershell
python tools/epg_replay.py "path/to/capture.log" --port COM4 --baud 115200 --station 40e231533233 --time 2024-11-12T04:30:00+01:00 --loop
```

Synthetic E2 suite and continuous PAD data (after generating its fixture):

```powershell
python tests/test_epg_synthetic_paths.py C:/msys64/ucrt64/bin/g++.exe
python -u tools/epg_synthetic.py --port COM4 --loop
```

The synthetic client tests MSC/PAD, reversed segments, repetition, cache return,
source isolation, legacy handoff and deliberate bad UA/identity/header/CRC.
Expected NACK DATA responses in negative cases are successful rejection tests.
Its default dataset has three programmes for `Synthetic EPG One` on
12 November 2024 and no logo. `--input` selects another length-prefixed group
fixture, `--report` selects the result path; Ctrl+C stops the loop and requests
LIVE. Unlike the RAW replay client, this client has no interactive console.
Close other serial monitors before either tool. The receiver's baud stays
115200; these clients do not flash firmware or write settings.

See [the UART testing guide](docs/EPG_TEST_CZ.md) for RAW replay/UI usage and
[synthetic path tests](docs/EPG_SYNTHETIC_PATHS_CZ.md) for MCU evidence and RF
limitations. Earlier hardware reports describe the build/date named in them;
current synthetic MSC and PAD tests have both passed on the receiver.

### PNG, Adam7 and slideshow diagnostics

An EPG logo must arrive as a named MOT PNG object referenced by the selected
service's SI metadata. Sending raw PNG bytes as an E1 group or setting STATION
alone cannot create a logo. STATUS distinguishes programme readiness from
`EPG LOGO bytes=N`; the default three-programme fixture contains no image.

Both EPG logos and slideshow support ordinary PNG and interlaced PNG (Adam7).
EPG validates the complete image before drawing into its 100×70 logo area;
the synthetic logo test used a 96×64 RGB image in both forms. The following
best-effort diagnostics accompany Adam7 EPG rendering:

```text
[EPG/LOGO] bytes=N interlace=1 validated=0|1 stage=... detail=... w=N h=N in=N out=N pass=N row=N pad=N
[EPG/LOGO] draw=0|1
```

`validated=1 stage=ok` and `draw=1` establish successful validation and drawing.
`pad` reports workspace alignment padding (4 bytes on the tested ESP32).
The ordinary PNG branch does not emit this Adam7 drawing result. The shared
slideshow renderer separately reports `[SLS/PNG] Adam7 validation=FAIL` or
`[SLS/PNG] Adam7 render=OK|FAIL size=N` when diagnostics allow it. The structured
`$M=SLIDESHOW=2` type is the same for normal/interlaced PNG; no PNG bytes or
interlace flag are added to that regular monitor message.

On 24 October 2026 all four synthetic MSC/PAD × normal/Adam7 logo combinations
loaded the correct PNG with zero CRC/invalid/UART errors. Both Adam7 paths
reported successful validation/drawing, and the user confirmed the final PAD
Adam7 logo on the display. This EPG test does not constitute a separate test
of reception/display through the slideshow collector.


### LIVE availability, trustworthy time and archive browsing (25 October 2026)

`EPG STATE` separates recent transport reception from PI date validity, SI/PI
presence and clock certainty. `EPG RX fresh` remains the selected PI cache age;
`EPG STATE transportFresh` describes valid MOT group arrival independently.
`sourceStart/sourceEnd` delimit original schedule-local calendar values from
Directory scope (epoch-style calendar arithmetic, **not UTC instants**); they
are zero when unavailable. PI start/actual instants stay unchanged UTC.
`archive=1` means explicit archive preview was requested. Standard LIVE can
also show original historical lists, clearly labelled Archive, without preview.

State priorities: conflict; no SPI; waiting for Directory; empty Directory;
pending identity; SI-only without PI; uncertain clock; historical/archive PI;
then ready or programme loading. Inspect the separate flags for simultaneous
conditions. Valid state names are:

| State | Meaning |
|---|---|
| NO_SPI | No confirmed SPI presence. |
| SPI_PRESENT_WAIT_DIR | SPI present, no valid complete Directory yet. |
| DIRECTORY_EMPTY | Valid Directory declares zero objects. |
| PENDING_IDENTITY | Current service not yet mapped unambiguously to SI/PI. |
| SI_ONLY | Station metadata available; no PI declared for this service. Own logo can still be received. |
| PI_DECLARED_LOADING | A matching PI is declared but a usable current view is not ready; this also covers expired transport freshness. |
| PI_DATE_STALE | Historical PI or explicit archive view; not a live NOW/NEXT promise. |
| READY_LIVE | Fresh programme with validated date/clock and NOW or NEXT. In mode=TEST, this uses the explicit virtual clock. |
| IDENTITY_CONFLICT | Conflicting SI/bearer; no arbitrary programme/logo selection. |
| TIME_UNKNOWN | PI exists but clock/offset is uncertain; NOW/NEXT and progress disabled. |

`EPG CLOCK encoding` is 0 Unknown, 1 Local, 2 UTC for the broadcast sample.
`valid` reports resolved UTC validity, while STATE clockValid additionally
requires confirmed current offset. `offset` is signed minutes, `age` is the
sample's monotonic age in milliseconds. These fields report the broadcast
clock even during TEST; STATE clockValid then refers to the virtual test clock.

AN649 defines DAB_GET_TIME type=0 as local and type=1 as UTC. The receiver keeps
main-screen radio fields local and queues a UTC read, followed by a confirming
local read. A bounded pair within 2 seconds supplies current half-hour offset
without using historical PI, EID/ECC timezone guesses or a global TZ variable.
Invalid/missing pairs, lost lock and clock samples at least 300 seconds old
cannot establish Ready. After lock loss, a new clock generation is required.
DEBUG logs bounded raw calendar bytes as `[DAB/CLOCK] type=0|1 ...` and the
confirmation's UTC/local/offset. The type=1 contract is documented and covered
by mocks; it still requires RF verification on the installed chip firmware.

PI timePoint preserves whether LTO was explicitly present; per TS 102 371,
absent LTO means schedule-local time is UTC, not an inferred CET +60. Original
PI LTO is used for historical programme display. The current broadcast offset
is used for current radio time. No 2024-to-2026 date/year mapping is performed.

Explicit archive browsing is available without ENABLE or DEBUG:

```text
EPG ARCHIVE 2024-11-12
EPG TEST STATUS
EPG ARCHIVE OFF
```

This requires an allocated Core in LIVE, not TEST. The date is exactly
YYYY-MM-DD and is an original schedule-local calendar day. It opens EPG with
ARCHIVE / SIMULATION marking, keeps broadcast UTC and all original PI instants,
and disables NOW/NEXT/progress. It does not simulate today's time of day on
that archive day. OFF restores standard LIVE date selection; historical data
still remains Archive if no current PI is broadcast. This feature is off by
default and resets with TEST/STOP/tune context changes. Replies/errors:

```text
EPG OK ARCHIVE preview; original dates; NOW disabled
EPG OK ARCHIVE OFF
EPG ERROR ARCHIVE requires LIVE data
EPG ERROR ARCHIVE use YYYY-MM-DD or OFF
EPG ERROR ARCHIVE date
```

The existing DEMO/E1/E2 test clock and TEST label remain available. New synthetic
fixtures use `tests/fixtures/epg/*_v2.bin`; older synthetic inputs are retained
as snapshots of the old erroneous encoder. Example:

```powershell
python -u tools/epg_synthetic.py --port COM4 --input tests/fixtures/epg/synthetic_programme_groups_v2.bin --loop
```

The original RAW capture and reference ZIP have not been altered. The reference
JSON's time values had the old bit-offset error, so the regression now verifies
UTC/LTO independently from original binary objects while retaining all other
reference comparisons. See [the LIVE report](docs/EPG_LIVE_TIME_REPORT_20261025.md)
and [Andrzej's manual checks](docs/EPG_ANDRZEJ_20261025.md).


### Separate packet and X-PAD transport diagnostics (25 October 2026)

`EPG TEST TRANSPORT` is read-only and available without ENABLE or DEBUG, in
LIVE and TEST. It does not start replay, query the chip, change ownership,
allocate Core, or clear programme data. STATUS still describes the active Core;
TRANSPORT additionally reports observations of the other transport.

```text
EPG TEST TRANSPORT
EPG OK TRANSPORT queued
EPG AUDIO ...
EPG PACKET ...
EPG RADIO ...
EPG OWNER ...
EPG TRANSPORT path=PACKET ...
EPG MOTPATH path=PACKET ...
EPG REJECT path=PACKET ...
EPG TRANSPORT path=PAD ...
EPG MOTPATH path=PAD ...
EPG REJECT path=PAD ...
EPG RESETS ...
EPG HEALTH ...
EPG TRANSPORT END
```

An active request returns `EPG OK TRANSPORT pending` instead of starting a
second output. Admission needs 64 bytes of UART headroom; below that the command
is handled silently and the client must retry. Pages wait for at least 512 bytes
of headroom and are emitted at most once per 100 ms, normally taking about 1.7 s.
There is no continuous HEX dump. A context epoch/reset change terminates output
with `EPG TRANSPORT CANCELLED context changed`; retry after tuning settles.
Pages are successive observations, not an atomic snapshot. Counters can advance
between lines. UART backpressure may extend the duration indefinitely.

| Record | Meaning |
|---|---|
| AUDIO | Current audio SID/full CID; raw UA identity, generation/publication and inventory completeness/failure; operational `usable` separately. |
| PACKET | Selected packet endpoint and discovery status, confirmed/unverified advertisements, tracked endpoint and probe phase. `confirmed/PAD; no packet` is a valid state. |
| RADIO | FM/DAB, requested tuner index/frequency in kHz, EID/ECC, lock, tune/switching, CTS/overflow/errors and cached XPAD set/get/match diagnostics. During tuning the requested frequency is not proof of lock there. |
| OWNER | LIVE/TEST and actual NONE/PACKET/PAD/TEST owner, UI channelIndex, epoch, actual Core resets/last reason, logical availability transitions and malformed-header count. |
| TRANSPORT | Per-path canonical endpoint, source-class header counts, SPI candidates/admissions/blocks, legacy handoffs, latest candidate raw header identity and metadata, wait/idle durations and Directory observation reason. |
| MOTPATH | Groups/envelope validity/CRC, type4/type6, verified complete Directories, bounded unique/repeat history, empties, last TID/body size/object count and owner-decoded SI/PI/PNG/programme count. |
| REJECT | Separate rejection counters, listed below. |
| RESETS | Actual runtime Core::reset calls by reason, listed below. |
| HEALTH | Heap-capability free/largest blocks, minimum free heap, dropped UART bytes, audio/SLS flags and active Core cache/error counters. |

`headers=a/b/c/d` means DATA_SRC 0/1/2/3. PACKET records source0; PAD records
source1/2 plus reserved source3 observations. Source0 is standard data, source1
is non-DLS PAD, source2 is DLS. Raw UA=0007 is an X-PAD SPI candidate **only on
source1**. Actual admission also requires verified current audio UA, matching
SID/CID/generation, valid state/physical metadata and payload bounds. Raw UA=0007
on source0 or source2 alone does not prove X-PAD SPI. Source1 Slideshow UA=0002
and DLS still go through their legacy paths. `last` preserves the latest SPI
candidate when candidates exist; legacy traffic does not overwrite it.
Full command CID and short DSRV CID are reported separately; only the router's
specific verified representations are accepted, not arbitrary global masking.

Audio `ua` is confirmed / unknown / failed / not-signalled. Raw `identity` is
0 Unqueried, 1 NotSpi, 2 Spi, 3 Failed, 4 Slideshow, 5 SpiAndSlideshow.
An unusable audio context or failed/incomplete scan does not revoke an already
confirmed PAD owner. A complete valid negative UA result does. Packet selection
retains a distinct packet endpoint even when the audio component advertises SPI.
`confirmed` counts SPI advertisements, including audio, not just packet endpoints.

Transport `admitted` counts groups accepted by the owner Core; it does not mean
Ready. `blocked` counts groups excluded from Core by ownership/replay. `legacy`
counts handoffs to the existing DLS/SLS path, not successfully decoded images.
Other-transport groups are observed without entering or resetting the active
Core. Observations of valid empty packet Directory therefore cannot clear PAD
SI/PI, even when the two streams use the same TID.

`dirs` means a complete validated Directory, not each type6 fragment. Without a
second reassembly buffer, the unowned path can directly validate a single-group
empty Directory or a known complete repeat; new nonempty/fragmented unowned
Directories remain undecoded. `unique`/`repeats` use a four-fingerprint bounded
history; `historyLimited=1` reports overflow, so these are not an unlimited count
of all distinct Directories ever seen. Packet empty 11A fixture: group 27 bytes,
Directory body 16 bytes, objects=0, TID=8CC2.

`reason` values are `not-owned` (no canonical endpoint), `waiting-data`,
`incomplete-directory`, `directory-timeout` (no verified Directory after 300 s),
`directory-known`, `empty-directory`. A signalled PAD with no Directory remains
WAIT_DIR even if packet keeps sending empty Directory. Timeout is diagnostic;
it does not automatically switch away from the PAD owner. DIRECTORY_EMPTY and
SI_ONLY UI messages explicitly describe the selected carousel, not the whole
multiplex. Time validity, archive and identity rules documented above still apply.

REJECT keys: `ua` unconfirmed/negative identity; `generation` stale/incomplete
context; `sid`/`cid` mismatch; `inactive` unusable audio; `dscty` unsupported data
type; `state` bad service state; `physical` chip error; `length` bounds;
`read` failed exact payload read; `replay` live data quarantined by TEST;
`owner` different active transport; `data` invalid first envelope or Core rejection;
`no-endpoint` no eligible packet context; `busy` another receiver operation.
Some failures occur before group observation, so header/rejection totals need not
match group totals. CRC diagnostics count inspected group CRC failures separately.

RESETS keys: `tune`, `service`, `packet-endpoint`, `packet->PAD`, `PAD->packet`,
`pad-endpoint`, `ua-revoked`, `replay-start`, `replay-stop`, `PAD-acquire`.
They count actual runtime Core::reset calls, not every duplicate signal or cache
replacement. NONE-to-PAD uses PAD-acquire. Reason counters are lifetime totals;
path counters are scoped to the context epoch and canonical endpoint. A new
endpoint clears only its path evidence; tune/STOP context clears both paths.
Counters saturate at UINT32_MAX. `stateChanges` counts logical availability
changes, not TFT redraws. In TEST, live frames remain excluded from the test Core.

HEALTH `core=0` means Core is not allocated; `arena` is its required Core-plus-buffers size,
not an assertion of allocation. `free8/largest8` and `free32/largest32` are the
separate heap capability classes, not quantities to sum as total memory.
`UARTdrop` counts lost diagnostic bytes, not lost RF packets. Audio/SLS flags do
not establish uninterrupted playback or visual correctness. Verify those on RF.

The bounded evidence registry occupies 516 bytes on the ESP32 target. There is
still one Core and no additional MOT/PNG framebuffer. Host A–F tests and a Czech
RF procedure are linked from [the transport report](docs/EPG_MUX11A_XPAD_REPORT_20261025.md)
and [RF instructions](docs/EPG_MUX11A_XPAD_RF_TEST_CZ.md).


### EPG acquisition timing and startup baseline (25 October 2026)

Automatic production packet acquisition explicitly requests a **500 ms**
baseline. Manual `DEBUG SPI` explicitly keeps **5000 ms** followed by its
30-second observation window. Both use the same cooperative START/STOP and
cleanup state machine. Log lines report the actual mode and baseline, for example:

```text
[DAB/SPI/TEST] mode=LIVE baseline=500 ms then START SID=... CID=...; observe=continuous; audio/SLS kept
[DAB/SPI/TEST] mode=DEBUG baseline=5000 ms then START SID=... CID=...; observe=30s; audio/SLS kept
```

The 500 ms deadline is the earliest eligibility for START, not a guaranteed
command submission time: pending radio work, lock/context validation and the
serialized scheduler still take precedence. X-PAD needs no separate packet START.

While `DEBUG` diagnostics are ON, bounded `EPG ACQ` records report first observed
acquisition milestones for each transport. No new command or ENABLE is required.
Turn DEBUG on **before** selecting the test station to capture the start.

```text
EPG ACQ path=PACKET sid=........ cid=........ base=first-observed t0=... t1=... t2=... t3=... t4=... t5=... t6=... t7=... t8=...
```

All numeric values are milliseconds from that path's first captured milestone;
subtract two known values to obtain a stage interval. `N/A` means unobserved or
not applicable, never zero delay. Unsigned timestamp subtraction is safe across
`millis()` rollover for the measured acquisition window. Each milestone is
captured only once per audio/endpoint context; lines can update as later stages
arrive. Paths are PACKET and XPAD with canonical transport SID/full CID.

| Point | First captured event |
|---|---|
| t0 | Audio switch reaches Ready or AudioOnly (settled service context). |
| t1 | Current validated transport endpoint published/validated for request. |
| t2 | Packet probe created; baseline begins. |
| t3 | Serialized packet START requested. |
| t4 | Packet START succeeds. Timeout/failure leaves this N/A. |
| t5 | Owner Core validates its first complete MOT Directory. |
| t6 | A complete selected PI yields cached programmes for the selected day, with no ambiguous identity. |
| t7 | EPG overview actually draws valid LIVE NOW or NEXT. Until opened/rendered this stays N/A. |
| t8 | Selected SI and own PNG logo are available together. SI-only can reach t8 with t6/t7 N/A. |

XPAD t2/t3/t4 are N/A because that transport rides the audio component. TEST
replay does not produce this LIVE timeline. Capturing can miss earlier events
when DEBUG was enabled late; no timestamps are backfilled from guesses. A tune,
service-context change, endpoint change or DEBUG off/on starts new evidence.
Changing only the browsed EPG station is not a new RF acquisition; interpret
selected-programme stages with the current STATUS station identity.

Output needs 320 bytes of UART queue headroom and emits at most one changed
path record per 250 ms. Unchanged records do not repeat. Trace storage is 124
bytes on ESP32, with no heap allocation or extra MOT/image buffer. CPU, FIFO and
audio effects still require live RF verification.

Relative to the old five-second production baseline, the expected startup
baseline reduction is **4500 ms**. The preceding local version already used
500 ms, so this refactoring does not claim an additional 4500 ms gain over it.
Total DIR/PI/UI latency depends on the broadcast carousel and has not been
measured on hardware for this version. See [the acquisition report](docs/EPG_PI_PRIORITY_REPORT_20261025.md).


### Multi-TID reception, SLS activity and receive health (26 October 2026)

The existing `EPG TEST TRANSPORT` command now includes `EPG IO`, `EPG SLS`
and two `EPG RX path=... slot=...` lines. No new ENABLE requirement or test-mode
switch is introduced. The existing STATUS `EPG RX unmapped=...` aggregate line
is unchanged; distinguish it from the per-slot TRANSPORT lines by their fields.
Output remains queued at most one page per 100 ms with 512-byte headroom,
ending in `EPG TRANSPORT END`; context changes still cancel the sequence.

`EPG IO` fields:

| Field | Meaning |
|---|---|
| reason / stalled | Recovery gate: NONE, CTS_NO_PROGRESS, CTS_GENUINE, SPI_TRANSPORT_ERROR. |
| failures | Consecutive terminal command failures, independent of host-gap classification. |
| known / lastCtsAgeMs | Whether an answering command has been observed in the current DAB generation; ignore the age if known=0. DeviceError is an answer as well as Ok. |
| inFlight / inFlightAgeMs / op | Outstanding command and its operation identifier; age=0 when none. |
| recovery | Recovery requests counted by the driver. This is not a count of completed physical resets. |
| IRQ | Rising observations of the DSRV status bit, not GPIO edge count. Repeated latched high snapshots are coalesced. |
| clearStart | Pending acknowledgements consumed when GET_DIGITAL_SERVICE_DATA starts. |
| remaining | Rearms from nonzero buffersRemaining. |
| newer | Rearms from a newer status sequence observed during the in-flight read/dispatch. |
| suspect | Bounded verification reads after a payload/error while the acknowledged status remains high; not proof of a lost IRQ. |
| drainMaxMs / errors | Largest command-start-to-dispatch drain interval, and malformed/physical/failed reply completions. |

The progress watchdog trips after at least **8 terminal failures spanning 5000 ms**
without an answering command, even when all timeouts are classified HostStarved.
An answering command resets the failure window. It does not trip on idle time,
a single delayed host service, or elapsed rendering time alone. Existing genuine
CTS protection also remains. A trip gates new DAB commands and uses the existing
sketch cold-recovery path, including shared tuner/TFT reset and its 60-second
retry limit. No reset is issued inside a reply callback. On a DEBUG-visible trip,
`[DAB/WATCHDOG]` prints the reason and relevant counters once. The last host gap
is diagnostic evidence only; it is not the entire timeout and does not establish
when CTS became ready. POLL and INTB use the same command-completion watchdog.

DSRV pending is consumed on start, then rearmed by remaining buffers or a newer
status sequence. A same-level sticky bit alone cannot create an unbounded drain
loop. A suspect/error allows one verification read; malformed replies wait 20 ms
before retry. An empty verification stops. New observed edges and genuine
remaining buffers continue draining within the existing four-command fairness
budget. Tune/stale-generation reset discards old pending state. Actual FW 6.0.9
IRQ/ACK behaviour still needs RF verification; these host checks do not identify
the cause of the tester's 11A CTS hang.

`EPG SLS` fields include current `tid`, `active`, packed `bytes`, declared `body`,
`SLS_DUP_VALID`, `SLS_DUP_CONFLICT`, `crcBad`, `SLS_LAST_ACTIVITY_AGE` and
`SLS_FIRST_SEGMENT_AGE`, in milliseconds. Ages are meaningful for an active
collector; the first-segment age also requires received body bytes. Counters are
lifetime diagnostic totals. A duplicate refreshes activity only after CRC,
current ownership/TID, segment length, packed contents, bounds and last-flag
validation. It adds no bytes and does not alter the bitmap. New MOT body CRC
failures are rejected as well. Completed/published objects and foreign/stale
sources cannot keep an incomplete collector alive. Identical headers alone do
not extend its lifetime. Actual silence still expires after 30 seconds.

Per-slot `EPG RX` fields:

| Field | Meaning |
|---|---|
| path / epoch / slot / tid | Active Core owner, context epoch and one of the two bounded slot identities. |
| state / mask | idle, receiving, ready, or decoding; segment bitmap in hexadecimal. |
| rx / duplicates / started | Admitted CRC-valid fragments, matching duplicates, and assembly attempts for that slot's retained TID. |
| received / decoded | Complete bodies with the declared size versus successfully decoded/validated PI/SI/PNG. Received does not imply valid TLV or displayed data. |
| skipped | Eligible groups deferred while this occupied slot contributed to capacity pressure; a deferred third TID is retried by the carousel. |
| timeout / evictions | Expired attempts, and the subset evicted for no progress under capacity pressure. |
| maxGap / age / progressAge | Largest interval between accepted fragments, age of the latest attempt and time since its last new segment. Matching duplicates refresh activity, not progress. |
| reason / highWater | 0 normal; 1 inactivity timeout; 2 no-progress expiry under pressure. highWater is the peak occupied slot count, at most 2. |

Counters belong to the retained TID in each slot and reset when a different TID
claims it. Only two slot identities are retained, not an unbounded TID history;
compare deltas only for matching path/epoch/slot/TID. Completed slot metadata
can remain visible with state=idle. Global SI/PI/PNG counts describe decoded
objects, so SI=0 alone does not establish that no SI fragments arrived.

The two 12 KiB blocks allow independent PI and SI reception. The single bounded
TLV pump pins its input block while the other receives; PNG uses a free block
once its own SI association is known. Three simultaneous 12 KiB bodies do not
fit and are deferred, never silently merged or overwritten. Five-second
preference changes do not reset RX fragments. Inactivity follows the existing
Directory-derived carousel timeout (120 s to 30 min, default 300 s). Under
observed capacity pressure, the same timeout without any new segment permits
an incomplete duplicate-only slot to expire. Finished programme/SI/logo cache
survives preference changes and identical Directory repeats.

EPG allocation remains one-time with rollback and separate capability-pool
checks: at least 12000 bytes DRAM and 4096 bytes executable 32-bit IRAM reserve.
`EPG HEALTH arena` now reports 63600 target bytes; heap and largest-block fields
must be measured on the actual unit. `stackMinBytes` is the calling task's stack
high-water free space, in **bytes** for this ESP-IDF port. There is no new image
workspace, PSRAM dependency, per-segment allocation, EPG flash or NVS storage.
The SLS 50 KiB buffer and shared 76800-byte decoder workspace remain unchanged.

Use [the English RF test guide](docs/SI4684_RX_RF_TEST_EN.md) for live acceptance.
Host simulation passing is separate from RF, audible playback, TFT and stack
acceptance on the physical receiver.


### Browsing directly in the NOW/NEXT overview

ROT1 rotation on the overview now previews NEXT and further cached programmes.
Rotate right to advance and left to return through earlier entries. Selecting
the actual NOW entry resumes automatic NOW/NEXT following. Bounds do not wrap.
If no timed NOW/NEXT is available, rotation browses stored entries without
claiming they are currently broadcast.

The preview uses the existing programme area. Actual NEXT is labelled NEXT;
other preview entries use the localized programme label. Following entries
are shown as programme information. Future/past previews have no live progress
bar and never change broadcast UTC, programme times, cache selection or tuning.
An overview-only movement repaints the programme region instead of the full
screen. Valid preview identity is retained across cache updates; a changed
station/day or removed entry returns to automatic following.

Click ROT1 to open the programme list at the previewed entry, then click again
for detail. The existing list/detail controls remain: ROT1 moves in the list
or scrolls detail text; ROT2 changes the list day or moves between detail
programmes. ROT2 does not move the overview. MODE back from the list, or closing
and reopening EPG, returns the overview to automatic NOW/NEXT. Menu/volume
suspension keeps the selected view.
