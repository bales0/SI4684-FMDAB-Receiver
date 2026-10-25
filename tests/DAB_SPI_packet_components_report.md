Current corrections and firmware are documented in [the stabilization report](DAB_SPI_fixes_20261009_report.md).
The build/hash below records the earlier packet-inventory task.

# DAB SPI discovery: complete component inventory and packet-MOT distinction

## Source and scope

Implemented after explicit user approval, against the actual working tree at
`C:/Dokumenty/GitHub/SI4684-FMDAB-Receiver`. HEAD remains
`a38950d660bda9feb4d525d8109d0a796120b3db` (`browser improvement`), plus the
uncommitted changes from the preceding tasks, including I²S OFF/32/44.1/48 kHz.
The task-named ZIP and timestamped sketch were not present in this workspace;
the current root `SI4684-DAB-Receiver.ino` and `src/` were used and preserved.
This is not a checkout of an older GitHub main or a claim that the ZIP was imported.
The preceding firmware SHA256 was
`DF5B48B8028E0C6D0A29733479AFDDAF62E69DB43D2150B3518240B166F8AAC1`.

This phase implements inventory, application identification and diagnostics.
No receiver was flashed or measured. Actual SPI/MOT programme decoding and
NOW/NEXT remain outside this phase.

## References verified before implementation

- Local [AN649 Rev 1.9](../Specification/AN649%20Programming%20Guide%20Rev%201.9.pdf):
  section 7.5.2/Table 14, pages 395–396; Tables 15–16, pages 396–397;
  section 7.6, page 398; GET_DIGITAL_SERVICE_DATA DATA_SRC descriptions.
  Table 16 specifically limits the documented UA block to X-PAD applications.
- [ETSI EN 300 401](https://www.etsi.org/deliver/etsi_en/300400_300499/300401/02.01.01_60/en_300401v020101p.pdf):
  FIG 0/13 application signalling associates an application with a component.
- [ETSI TS 102 371 V3.3.1, section 7.1.1](https://www.etsi.org/deliver/etsi_ts/102300_102399/102371/03.03.01_60/ts_102371v030301p.pdf#page=25):
  SPI application signalling uses FIG 0/13 and the registered EPG application type.
- [ETSI TS 101 756 V2.4.1, Table 16](https://www.etsi.org/deliver/etsi_ts/101700_101799/101756/02.04.01_60/ts_101756v020401p.pdf#page=20):
  application 0x007 is SPI, 0x002 is SlideShow; transport MOT alone is not an application identity.
- Existing `src/vendor/si468x/Si468x.h`: `DabServiceListParser`, service/component
  entries and exact-CID semantics. Host tests compare every decoded field against it.

The available AN649 and vendor API do not establish a general method for obtaining
packet-mode FIG 0/13 UA signalling from FW 6.0.9. No private command or unsupported
API was invented. A successful 0xBB response with no SPI UA cannot exclude SPI
on a packet-MOT component. A failed 0xBB response makes discovery incomplete.
Positive reception and any broader 6.0.9 capability need separate evidence.

## Inventory and transactional validation

The actual reply starts at `SPIbuffer[1]`; four status bytes precede the list.
SIZE is therefore at `[5..6]`, VERSION at `[7..8]`, number of services at `[9]`
and the first service at `[13]`. Validation receives `SPIbuffer + 5` with
`replyLength - 4` bytes. SIZE excludes its own two bytes. Exact declared length,
eight-byte list header, maximum size 2694, N < 32, 24-byte service records,
NUM_COMP < 15, component bounds and both destination capacities are checked.
Trailing bytes and truncation fail validation before either inventory changes.

The small pure validator uses the same verified field extraction as the vendor
parser, with exact SIZE/capacity checks. It fits the existing transactional
station-table pass and avoids provisional callbacks or a second large buffer.
The vendor parser is the independent host-test reference, not another runtime table.

Service Info 1 is retained in the former padding byte of each eight-byte
`UaService`: P/D is bit 0 (0=programme, 1=data); PTY/linking bits remain available.
SID, first-component offset, NUM_COMP and wire component order remain retained.
List version is stored once. Optional service CAId/LOCAL/charset fields were not
added to the inventory; the established station-label charset path is retained.

Each exact 32-bit little-endian CID is retained unchanged in `dabUaComponents[]`.
Its own four bytes remain the sole stored source of component metadata:

| Field | Extraction |
| --- | --- |
| TMID | raw 16-bit component field bits 15:14 |
| ASCTy/DSCTy | component byte 2 bits 7:2 |
| DGFlag | TMID=3 and raw bit 13 |
| Primary | byte 2 bit 1 is set; zero means secondary (ETSI correction 2026-10-09) |
| CA | byte 2 bit 0 |
| UA info valid | byte 3 bit 0 |
| Reference | raw low 12 bits for TMID=3; otherwise low 6 bits |

The reference is diagnostic; commands continue to receive the complete CID.
For example `0x01FE0008` remains `0x01FE0008`; its P/S bit decodes to primary,
as in the vendor parser. A packet-MOT candidate requires TMID=3 and DSCTy=60.
DG and UA-valid are reported independently and are not SPI identity shortcuts.

Malformed lists retain both the old station table and component inventory,
invalidate in-flight UA ownership and selected endpoints, and publish Incomplete.
The last confirmed presence can remain cached, but supplies no usable endpoint.
Invalid-inventory diagnostic counts refer to the last valid retained list.
A new valid list is required before another meaningful UA-only pass.

Changed version, SID/CID topology or P/D invalidates the UA generation. Identical
periodic inventories do not restart the dump. Same-mux audio selection also
invalidates an in-flight UA pass without changing audio START/STOP timing.
Old answers cannot complete the new pass. The periodic 60-second discovery
cycle and DEBUG-independent detection remain in place.

## Discovery and safe selection

`Result.confirmed` and `confirmedPresent()` represent positive UA evidence.
`Result.packetMotUnverified` counts packet-MOT components without confirmed SPI.
`Result.available()` represents a selected usable endpoint, independently.

| Result | Meaning and action |
| --- | --- |
| Pending | New pass has not completed; no endpoint is selected. |
| Confirmed/service or Confirmed/ensemble | Complete successful pass with a unique confirmed endpoint according to the existing active-service priority. |
| Ambiguous | Positive SPI evidence exists but equivalent confirmed endpoints prevent safe selection. |
| NotSignalled | Valid complete inventory and successful UA answers, with neither SPI nor unverified packet-MOT. SPI was not found in the available signalling. |
| PacketMotUnverified | At least one TMID=3/DSCTy=60 component lacks confirmed SPI; no endpoint is selected. |
| Incomplete | Invalid inventory or failed/malformed UA response prevents a reliable conclusion and endpoint selection. Other positive evidence can remain visible. |

Validated UA 0x007 is confirmation. Mixed 0x002/0x007 replies retain both
identities. No label, DSCTy, CID order or arbitrary UA data byte is treated as
proof of SPI/profile. A unique confirmed endpoint on the active SID takes
precedence; otherwise a single confirmed ensemble endpoint can be selected.
Unknown packet-MOT never competes with confirmed candidates as an endpoint.

`spiPresent()` continues to drive the unchanged EPG artwork, position, font,
colors and redraw behavior. It means confirmed signal, including ambiguous
confirmed cases. PacketMotUnverified does not turn it on. During a rescan or
an unreadable update, the last known confirmation may remain cached; a valid
topology/version change or retune clears it. No actual programme availability
or coverage of the currently selected audio service is asserted.

`DEBUG SPI` remains an explicit bounded START/STOP probe. Its existing
`available()` gate prevents unverified, ambiguous or incomplete selection.
No automatic SPI START was added and Route::Spi payload handling is unchanged.

## Slideshow data selection

`resolveDabDataService()` no longer selects the first type-3 row or an anonymous
ensemble-wide fallback. Its separate policy requires a completed valid UA pass,
a unique non-CA packet-MOT component on the active SID and an explicit pure
SlideShow identity. Explicit X-PAD slideshow on the active audio CID prevents
replacement by a standalone component. Unverified MOT, SPI, mixed SPI/SLS,
foreign-SID components and ambiguous standalone SLS are excluded.

When a completed pass reveals such an eligible standalone SLS endpoint, the
existing data-service evaluation path is notified. No audio timing, DSRV
priority/CTS/IRQ code, `getServiceData`, MOT collector or SLS cache was changed.
An installation that relied on guessing a standalone SLS service from type 3
alone will now keep audio/X-PAD and leave that unproven endpoint unstarted.
Real standalone-SLS identity/API behavior still needs hardware verification.

## Diagnostics and actual synthetic-host output

Inventory, packet candidates and summaries share `availableForWrite() >= 256`
and the existing 100 ms deadline. At most one such line is emitted per scheduler
pass. UA completion waits for the final committed answer; queued output is independent
following the 2026-10-09 stabilization fix.
`audio/data` counts services by P/D; it does not count component transport modes.

The following lines were captured from the host harness running the actual
production parser/discovery/logging bodies, not from Si4684 hardware:

```text
[DAB/INV] services=10 components=10 audio=10 data=0 packetMOT=0 invalid=0 version=7
[DAB/UA] scan complete services=10 components=10 processed=10 failed=0
[DAB/SPI] confirmed=0 packetMOTUnverified=0 status=not signalled selected=0 SID=00000000 CID=00000000 profile=unknown

[DAB/SVC] SID=E1234567 PD=1 components=1 version=7
[DAB/COMP] SID=E1234567 i=0 CID=00F2E321 TMID=3 DSCTy=60 DG=1 PS=primary CA=0 UAvalid=0
[DAB/SPI] packet-MOT candidate SID=E1234567 CID=00F2E321 identity=unverified
[DAB/INV] services=11 components=11 audio=10 data=1 packetMOT=1 invalid=0 version=7
[DAB/UA] scan complete services=11 components=11 processed=11 failed=0
[DAB/SPI] confirmed=0 packetMOTUnverified=1 status=packet MOT unverified selected=0 SID=00000000 CID=00000000 profile=unknown
```

Full synthetic output is generated at `.pio/tests_dab_packet/synthetic_runtime.log`.
The supplied audio/SLS captures remain consistent with a negative finding in
that available signalling; they do not establish absence on other multiplexes.

## Files changed in this task

- `src/dab_component_inventory.h` (new): exact-field views and bounded transactional list validation.
- `src/dab_spi_discovery.h`: independent confirmation/transport counts, precise states,
  mixed UA identity and the isolated conservative SLS selector.
- `src/si4684.h`: compact P/D retention, version/validity state and invalidation helper.
- `src/si4684.cpp`: transactional validation, classified throttled diagnostics,
  discovery publication, stale-UA invalidation and conservative data resolution.
- `tests/test_dab_spi_discovery.cpp`: revised status/UA assertions; prior scan tests retained.
- `tests/test_dab_packet_inventory.cpp` (new): requested synthetic cases, bounds,
  exhaustive vendor-field agreement and SLS isolation.
- `tests/test_dab_packet_runtime.py` (new): compile and execute actual production
  function bodies with mocked UART/time; capture logs and assert wiring safety.
- `tests/README.md`, `Serial_protocol.md`: commands and current diagnostic semantics.
- `tests/DAB_SPI_packet_components_report.md` (new): this report.

FM/RDS, NVS, menu, I²S settings and GUI sources received no changes in this task.
All existing uncommitted changes from prior tasks were retained. No commit was made.

## Validation

- Strict C++11 policy/inventory suite: PASS for audio/SLS-only; separate unverified
  packet-MOT; SPI on the last CID; mixed confirmed and unknown MOT;
  ambiguity; failed UA; malformed/truncated lists; component/service capacity;
  all 65,536 raw-field values compared to vendor decoding; SLS exclusion cases.
- Actual-function host harness: PASS for reply offsets, stable inventories,
  last-response completion independent of UART output, unchanged UI/retained CID after errors,
  list-version and audio-reset stale generations, UART headroom/100 ms pacing,
  zero-component logging and no SLS request from unverified/SPI endpoints.
- Existing scheduler, FM features, service-switch, user-application, discovery
  and probe/router suites: PASS. I²S mode/NVS and actual configuration tests: PASS.
- `pio run -e esp32dev`: PASS; existing TFT_eSPI TOUCH_CS warning remains.
- Static RAM: **62,240 B**, **+8 B** from 62,232 B. The service-entry padding is
  reused, CID/identity arrays and 51,200 B SLS buffer are unchanged. No dynamic
  receiver buffers/containers were added. Existing transactional parser stack
  arrays remain; a small header and scalar component views are added.
- Flash: **3,456,561 B**, **+712 B** from the preceding firmware.
- `git diff --check`: PASS.

Firmware: `.pio/build/esp32dev/firmware.bin`, application-only at **0x10000**.
SHA256: `984E65D77382227B14183063BD8797A0DA4749F92E66C818F04508C77245D278`.

Final source SHA256:

```text
F89E52EDB175BF177CCE02EF691E2B61175DEA7ED07B6F42DCECA09725F746BC  src/dab_component_inventory.h
223ECC22CA361D1119D2E461BBCC032BBC51BA30A964FAD48ABEB820C7C4F546  src/dab_spi_discovery.h
83FA4FE21396CAB4B03360504431E43E9D621037122E1303B65FF2015369C204  src/si4684.h
BF33647A39CF1C42E4835FC518E347CAB062D0C1B0D63EDF229861C4357A0063  src/si4684.cpp
```

## Pending hardware verification

1. Flash the application-only `firmware.bin` at 0x10000 to preserve NVS, enable
   DEBUG, and capture a complete inventory/UA summary for the supplied audio/SLS
   multiplex. Confirm grey EPG, audio, infotext and complete SLS image reception.
2. Capture a multiplex with packet-MOT. Confirm full SID/CID, P/D, TMID, DSCTy,
   DG and UA-valid flags; unknown MOT must leave EPG grey and start no new service.
3. Find a positive 0x007 broadcast and verify cyan EPG plus exact selected
   endpoint or explicit ambiguity. Actual SPI support is not established by
   the synthetic positive test and programme decoding is not implemented here.
4. During scans, rapidly switch Rotary1/Rotary2 and multiplexes. Verify no old
   UA endpoint, collector mixing, stale names or repeated data STARTs. Test
   DEBUG off/on and detection beyond 60 seconds.
5. With active SLS, inspect receiver overflow/error counters, CTS host/genuine
   timeouts/recovery and UART dropped messages; healthy targets are zero.
   Measure heap free/maxblock and stack high-water before/after. Any deviations
   need analysis; no real RF, clocks, heap or runtime coexistence measurements
   were made in the host tests.
6. Check INTB/AUTO/IR fallback, overlays, volume/mute, FM/RDS and I²S selections.
   Use DEBUG SPI only on a verified unambiguous endpoint, retaining existing
   safe probe timing/cleanup checks. Do not probe an unverified packet candidate.
