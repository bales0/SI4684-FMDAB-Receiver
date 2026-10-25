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

Lines beginning with `[` may appear when diagnostics are enabled. A structured
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
`DEBUG SPI STOP`, all `EPG TEST ...` commands and E1/E2 fragments remain
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
selection invalidate in-flight UA ownership. Routine refreshes keep the last
confirmed icon color and do not repeat an unchanged inventory dump.

The main display shows only `EPG` below the codec. Grey means EPG has not
been detected. Cyan means validated SPI presence was found, but the selected
station/day has no fresh unambiguous cached programme. Green means that
programme data for the selected station/day is cached and fresh (15 minutes).
Green can remain during an RF outage while the cached programme is still valid.
The detector retains known presence during routine rescans and clears on retune
or a valid topology/version change. In UART replay, presence follows the
validated test directory and readiness follows the test station/day cache.
The indicator is hidden in FM. It does not promise complete schedule coverage.
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

STATUS has no final end marker. It normally emits these six lines in order,
plus `EPG INJECT` as a seventh line during injection:

```text
EPG OK STATUS mode=LIVE|TEST frames=N crcBad=N invalid=N dup=N conflict=N incomplete=N busy=N
EPG DATA dirs=N objects=N PI=N SI=N PNG=N programmes=N cached=N limited=0|1 ambiguous=0|1 now=N next=N
EPG RX unmapped=N oversize=N timeout=N fresh=0|1 continuityJumps=N unsupported=N
EPG RAM arena=N free=N min=N largest=N audio=0|1 SLS=0|1 UARTdrop=N rxCRC=N rxOrder=N rxIdentity=N
EPG LOGO bytes=N interlace=0|1
EPG POOLS free8=N largest8=N free32=N largest32=N
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

The Core arena on the tested ESP32 is 50,800 bytes. Capacity is shared across
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
