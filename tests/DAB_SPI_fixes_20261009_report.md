# SPI discovery stabilization, 2026-10-09

Changes apply to the current working tree, preserving earlier approved audio,
SLS/UI and I2S work. The targeted [diff](DAB_SPI_fixes_20261009.patch) compares
against a copy taken immediately before this approved change, rather than HEAD.
No firmware was flashed and no RF/hardware test was performed.

## Proven corrections

Both component parsers now interpret P/S=1 as primary. CID 01FE0002 is primary;
01FC0002 is secondary. Full command IDs and all other metadata are preserved.
The old AN649-based interpretation was wrong. [ETSI EN 300 401 V2.1.1,
section 6.3.1, p.46](https://www.etsi.org/deliver/etsi_en/300400_300499/300401/02.01.01_20/en_300401v020101a.pdf)
specifies this polarity independently of the vendor guide.

Scan::functionalComplete checks the inventory, committed success/failure counts
and active command, without a diagnostic argument. Query scheduling, result
publication and standalone SLS evaluation no longer wait for diagnostic queues.
The complete Result, published generation, presence, dirty state and 60-second
refresh deadline are committed in foreground before the logger runs. Dirty=false
prevents repeated finalization. Selection remains conservative: only validated
UA 0x007 supplies SPI evidence; unknown packet-MOT cannot activate the icon.
Ambiguity, failures and last-known presence after invalid inventory retain their
existing semantics. No SPI START or full EPG decoder was added.

DEBUG toggling queues diagnostics only; it no longer restarts queries. Summaries
reference the immutable published generation, and resets drop unfinished summaries
and the UA diagnostic slot. Inventory lines use the retained validated inventory.
One pending UA dump is preserved until drained; newer diagnostic copies are skipped
while that slot is occupied. Every response still updates functional discovery.
This intentionally trades exhaustive DEBUG dumps for uninterrupted discovery.
No dynamic allocation or additional payload buffer was introduced.

## Diagnostic findings and documentation discrepancy

Local **AN649 Rev.1.9**, `Specification/AN649 Programming Guide Rev 1.9.pdf`:
DAB section 4.3, GET_DIGITAL_SERVICE_DATA response p.170, and common section 7.7,
Table 19 p.400, identify bytes 16-17 as RFU. These are not FM tables.
Property 0xB400 spans pp.224-225: bits 0/1/2 enable DLS/MOT/TDC; bits 15:3
are reserved.

[AN649 Rev.2.0](https://www.skyworksinc.com/-/media/Skyworks/SL/documents/public/application-notes/an649.pdf)
contains an internal discrepancy: its **DAB section 4.3**, command 0x84 response,
pp.176-177, names RESP16-17 UATYPE and links it to DAB_XPAD_ENABLE. But common
**section 7.7, Table 19, p.426**, still names DATA16-17 RFU. The task's claim
that section 7.7 itself defines UATYPE is therefore inaccurate for this PDF.
The FM/HD command table also retains RFU; it was not used as DAB evidence.

Rev.2.0 property 0xB400, p.239, assigns bits 0=DLS, 1=RFU, 2=SLS, 3=BWS,
4=TPEG, 5=DGPS, 6=TMC, 7=EPG, 8=DAB Java, 9=DMB, 10=IPDC, 11=Voice,
12=Middleware, 13=Journaline, 14=reserved, 15=other applications.
Thus 0x0097 enables 0,1,2,4,7, with different meanings in the two revisions.
The configured value is preserved. Its actual SET_PROPERTY result is now
retained and reported when DEBUG is enabled:

```text
[DAB/XPAD] value=0x0097 setResult=0 rev=unverified
```

0 means vendor Result::Ok; -1 means no recorded write attempt. Actual receiver
success still requires this hardware log; a successful host build cannot prove it.

DsrvHeader exposes only `rawUatypeOrRfu=readLe16(r+16)`. Under DEBUG a raw line
is allowed once per five seconds with at least 256 bytes of UART headroom:

```text
[DSRV/UA] raw16_17=0x0002 rev=unverified src=1 sid=00002FD9 cid=0000002A
```

This example is illustrative. Raw values never influence routing or presence.
Byte count, segment number, segment count and payload remain at 18/20/22/24.
The legacy DLS/SLS admission and payload handling, buffersRemaining drain and
overflow counter are unchanged. No CTS, IRQ, HOST SPI or TFT bus changes.

## Unverified FW 6.0.9 behavior

The local firmware catalog identifies the DAB 6.0.9 image, but no release note or
captured RESP16-17 data in this project proves its UATYPE contract. The supplied
UA dumps prove SLS advertisements, not that this raw DSRV field supplies UATYPE.
Simulated 0/2/7 parser fixtures do not establish firmware capabilities. EPG
reception and standalone SPI coexistence still need positive on-air testing.

## Verification

Passed with strict host C++11 `-Wall -Wextra -Werror -pedantic`:

- test_scheduler, test_fm_features, test_dab_service_switch.
- test_dab_user_application, test_dab_spi_discovery, test_dab_spi_probe.
- test_dab_packet_inventory: independent P/S expectations, both vendor wire
  fixtures, unchanged fields, exhaustive 65,536 vendor/helper comparisons.
- test_dab_dsrv_header: 0/2/7/ABCD raw fields, all short lengths/null input,
  unchanged byte count/segments/payload and sources 0/1/2 using production router.
- test_i2s_audio with production NVS and its existing host stubs.

Passed Python integration tests:

- test_dab_packet_runtime.py compiles actual parser, reset, publication, logger
  and SLS resolver bodies. DEBUG off/on, UART zero/fast/255 headroom, immediate
  last-answer publication, three delayed summary stages, final query failure,
  generation changes in scan/dump, DEBUG toggling without query restart,
  60-second deadline and no double finalization, retained invalid inventory,
  negative mux and packet candidate, blocked-UART SLS evaluation exactly once.
  A source assertion also checks that the production query scheduler has no
  diagnostic queue gates. The test manually settles tuner replies; it does not
  emulate the full radio transport or physical UART timing.
- test_i2s_integration.py: existing configuration/menu/failure-path regression.

`pio run -e esp32dev`: SUCCESS. RAM **62,256 B** (previous 62,240, +16);
flash **3,457,149 B** (previous 3,456,561, +588). MOT workspace stays 51,200 B.
The existing TFT_eSPI warning about undefined TOUCH_CS remains.

Application image: `.pio/build/esp32dev/firmware.bin`, address **0x10000**.
SHA256: `1260CAA47AABCED48A004C565E413758165988ED68A2D123CE492D94165C8A28`.
The merged receiver-complete.bin at 0x0 replaces NVS; use the application image
when retaining settings. No upload was performed.

## Hardware checklist and P/S verification

1. Load the application firmware using the normal receiver flashing procedure.
   In the 115200 serial console send `DEBUG` if diagnostics are off; wait for
   `[DEBUG] diagnostics ON`. The command toggles, so do not send it twice.
2. Find `[DAB/COMP] ... CID=01FE0002 ... PS=primary ...`. Compare the same SID/CID
   against an earlier log: only P/S interpretation should change. Different
   receivers/muxes may have different CIDs; any byte-2 bit 1 set means primary.
   The 01FC0002 secondary counterpart is a host fixture, not an expected station.
3. Verify continuous audio, dynamic infotext, and a completed/new JPEG or PNG
   slideshow. Monitor signal while loading and switch stations/multiplexes.
4. The supplied negative mux should keep EPG grey, `confirmed=0`, `not signalled`.
   A packet-MOT candidate alone must keep it grey. Confirmed signalling is not
   decoded EPG; positive on-air testing is still required separately.
5. Check XPAD setResult and optional raw DSRV lines. Compare CTS timeout/recovery,
   DSRV overflow and UART dropped counters; healthy runs should add zero.
6. Leave reception running beyond 60 seconds: one further low-priority UA pass
   per completed cycle, without doubled periodic queries. Repeat with DEBUG off
   and on; hardware behavior under UART congestion is not yet measured.

## Files changed by this task

- src/dab_component_inventory.h
- src/dab_spi_discovery.h
- src/si4684.h
- src/si4684.cpp
- src/vendor/si468x/Si468x.h
- tests/test_dab_spi_discovery.cpp
- tests/test_dab_packet_inventory.cpp
- tests/test_dab_packet_runtime.py
- tests/test_dab_dsrv_header.cpp (new)
- Serial_protocol.md
- tests/README.md
- tests/DAB_SPI_packet_components_report.md (corrections; old build retained)
- tests/DAB_SPI_fixes_20261009_report.md (new)
- tests/DAB_SPI_fixes_20261009.patch (new; targeted review artifact)
