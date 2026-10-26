# SI4684 receive-path RF acceptance guide

Applies to the SLS duplicate, two-slot EPG, CTS watchdog and DSRV pending changes
of 26 October 2026. **New live hardware tests have not been performed.** Host
simulation and a successful build do not establish RF/audio/TFT acceptance.
Installing a new firmware is a separate action requiring the owner's approval.

## Preparation

Use one UART client at 115200 baud, 8-N-1, LF/CRLF. Stop any PC replay/synthetic
sender. Check `EPG TEST STATUS`: it must say LIVE. Only if it says TEST, send
`EPG TEST STOP` once; STOP clears the test cache. Do not repeatedly clear cache
between LIVE measurements. Record firmware, time, channel, EID/ECC, audio SID/CID,
packet endpoint, signal quality and the actual observation duration.

Use the radio controls to enter DAB MAN mode. UART tuning, if required:

```text
ENABLE=1
TUNE=24
SERVICE=<actual list index>
```

In the current frequency table, index 24 is 11A / 216928 kHz and index 25 is
11B / 218640 kHz. Verify with `:FREQ`. SERVICE takes a current list index from
`$L`, not a hexadecimal SID. Select a stable station and do not retune within
a measurement window.

## 11A: cold boot and communication stability

1. Start with DEBUG OFF. Verify the state with the toggle response rather than
   assuming it. Listen for **more than five minutes** on one station. Check
   audio, DLS, slideshow, signal updates, volume and rotary/button response.
2. Enable DEBUG and confirm `[DEBUG] diagnostics ON`. Send:

```text
EPG TEST TRANSPORT
EPG TEST STATUS
```

Wait for `EPG TRANSPORT END` (normally about 1.7 seconds). If output says
CANCELLED, wait for tuning to settle and repeat. Repeat these two commands about
every 30 seconds for another stable window; they are queued and paced.
3. In `EPG RADIO`, check actual mode/frequency/lock/tune state, CTS, overflow and
   errors. In `EPG IO`, compare failures, recovery, reason, IRQ, clearStart,
   remaining, newer, suspect, drainMaxMs and errors. IRQ means observed DSRV
   status transitions, not physical INTB edge count. Repeated high snapshots
   must not create an endless drain. A suspect verification is not proof that
   the firmware lost an IRQ.
4. A valid PACKET Directory with objects=0 is legitimate DIRECTORY_EMPTY.
   Previously reported 11A traffic used E231B3FE/01F2C00B, TID8CC2, a 27-byte
   group containing a 16-byte Directory. Verify the current identities rather
   than forcing these values. Empty packet data does not establish the absence
   of EPG elsewhere in the ensemble.
5. For the selected audio service, distinguish confirmed UA=0007 SPI from
   UA=0002 slideshow. Only DATA_SRC=1 with validated audio SPI identity proves
   a PAD SPI candidate; DATA_SRC=0/raw7 does not. Previous samples from one
   service showing only slideshow do not rule out SPI on other services.
6. After the stable window, switch audio stations cautiously and repeat.
   Check that audio and controls recover and that the tuner does not remain
   permanently busy or Failed. Test each available control mode (POLL and INTB)
   using the radio's existing setting; keep separate observation windows.

If a genuine hang occurs, retain the last diagnostics and the
`[DAB/WATCHDOG]` line. Eight terminal failures spanning at least five seconds
without an answering command must request recovery even if all are labelled
HostStarved. One late host service or one timeout must not reset the radio.
An answering Ok/DeviceError clears the failure window. The last host gap is
not the full timeout and cannot establish when CTS was ready.

Observe the existing cold recovery: audio/service selection, shared tuner/TFT
reset and display restoration. Repeated recovery attempts must be at least
60 seconds apart. `EPG IO recovery` counts requests, not successful physical
resets. Verify actual restoration rather than accepting this counter alone.
Do not repeatedly power-cycle the unit before collecting the failure context.

Optional packet isolation: after the normal measurement, `DEBUG SPI STOP`
requests cleanup and pauses automatic packet reception until the next tune.
Observe whether audio/PAD slideshow remain stable without packet acquisition.
Retune and reselect the station for the normal packet-enabled comparison.
Do not use this isolation interval as proof of normal EPG performance. A
packet-without-parser comparison requires a separate reviewed diagnostic build;
there is no new UART command that claims to provide it.

## SLS: duplicate activity and image ownership

Watch `EPG SLS`: tid, active, bytes/body, SLS_DUP_VALID, SLS_DUP_CONFLICT,
crcBad, SLS_LAST_ACTIVITY_AGE and SLS_FIRST_SEGMENT_AGE. Ages are milliseconds;
the first-segment age applies only while receiving body bytes.

If reception naturally waits with one or more missing segments, identical
CRC-valid repeats should raise SLS_DUP_VALID and reduce activity age without
changing byte count. The collector should survive beyond 30 seconds and finish
when the missing segment arrives. Conflicting bytes/length/last flag, bad CRC,
foreign identity and repeats of a published image must not refresh that partial
object. Real inactivity still expires after 30 seconds. Headers alone do not
extend this timer. If this situation does not occur on RF, leave the RF duplicate
case untested; the host test independently reproduces a 90-second case.

Open/close slideshow, change volume and operate the radio while receiving it.
Confirm that the published single image is preserved until the UI releases it.
Check JPEG baseline/progressive, PNG and Adam7 where broadcast samples exist;
do not mark an unavailable format as tested.

## 11B: programme, SI and own logo together

Select Trójka or another service with declared PI. Observe several complete
carousel rotations, then repeat station/day/menu navigation.

Use `EPG RX path=... slot=...` to distinguish receiving fragments from decoded
objects. Two slots can hold PI and SI independently, including gaps longer than
five seconds and reversed segment arrival. `received` means complete declared
body size; `decoded` means successful parsing/validation. SI=0 alone does not
mean that no SI fragments arrived. Once SI identifies the station's PNG, a free
slot can receive that logo. Three simultaneous 12 KiB bodies cannot fit: the
third waits for a free slot and another broadcast opportunity.

Confirm completed programme/SI/logo cache survives identical Directory repeats,
preference changes and later logo reception without duplicate programme rows.
For Radio Kraków/OFF RadioKraków, SI and an own logo without PI are valid SI_ONLY.
Historical 2024 PI must remain archived/stale against a 2026 radio clock; it must
not become current NOW/NEXT just because it was received recently. Use test mode
with its virtual clock only as a separate UI test, never as a LIVE RF result.

If PAD SPI is actually confirmed, compare it against simultaneous packet traffic.
Empty packet Directory and packet START/STOP must not clear PAD cache. Switching
real source identity must reset both partial slots so equal TIDs cannot mix.

## Memory and acceptance checks

Capture `EPG HEALTH` before/after warm-up, slideshow display, EPG browsing and
station changes. Compare free8/largest8, free32/largest32, min, UARTdrop and
stackMinBytes. Do not add the capability pools together as independent total
RAM. stackMinBytes is free stack high-water space in bytes on this ESP-IDF port.

The target Core requires 63600 bytes: 14448 bytes metadata plus 49152 bytes of
buffers. The extra RX block is 12288 bytes; the old SLS 50 KiB buffer and shared
76800-byte decoder workspace are unchanged. Allocation checks retain 12000-byte
DRAM and 4096-byte word-only IRAM reserves; a memory error must leave the radio
usable and roll back partial EPG allocation. Verify actual unit margins and
absence of a progressive loss of heap/largest blocks rather than copying host
mock heap values.

Per-slot counters retain only the latest TID occupying each slot. Compare matching
path/epoch/slot/TID, and distinguish activity from progress. Directory-derived
inactivity is 120 seconds to 30 minutes, default 300 seconds. Under capacity
pressure, a duplicate-only slot with no new segment for this interval may expire
with reason=2; ordinary inactivity is reason=1. A five-second preference change
alone must not cause an eviction.

Acceptance requires stable audio/DLS/SLS/TFT/controls, advancing DSRV reception,
no stuck CTS retry loop, correct cache/source/date behaviour, bounded recovery
and adequate measured heap/stack. Record each unavailable or unperformed case
as NOT TESTED. Disable DEBUG after collecting the observations.

For exact UART field meanings, see [Serial_protocol.md](../Serial_protocol.md).
