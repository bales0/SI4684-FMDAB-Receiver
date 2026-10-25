# DAB XPAD readback and raw UATYPE diagnostics — 2026-10-09

This approved task extends the current working tree. The existing P/S fix,
UART-independent discovery, EPG artwork and raw 24-byte DSRV parser were already
present. No separate library repository, vendor sync or older ZIP was used.
The [targeted diff](DAB_XPAD_UATYPE_diagnostics.patch) compares with a snapshot
made immediately before these changes, preserving earlier uncommitted work.

## What changed

`configureDabXpad()` runs once in the DAB-only initialization branch, after
image/function verification. It writes the unchanged 0x0097, then reads B400
only if SET_PROPERTY returned Ok. Calls use the existing sequential boot API;
the read has a 250,000 us limit. No synchronous reads occur in Update, the
periodic scheduler, DEBUG toggles or FM. No new runtime command or competing
SPI transaction was introduced. A failed read does not fail initialization;
the existing command-diagnostic accounting still records its error.

A small record retains separate SET/GET Result codes, the measured value and
validity. A successful mismatch is valid evidence with match=0, not an error
silently rewritten to the requested mask. On error, any value written by the
transport is discarded. GET is not attempted after a failed SET. Every begin,
including early failures, and hardware reset/recovery clear the record. An
ordinary retune may retain it because the running image/property is unchanged.
DAB-to-FM-to-DAB reinitialization cannot reuse an old readback.

The existing paced DEBUG logger prints the record with actual GET_FUNC_INFO
firmware text. It does not query the radio. Success example (expected format,
not a newly measured hardware result):

```text
[DAB/XPAD] requested=0x0097 setResult=0 readback=0x0097 getResult=0 match=1 fw=6.0.9 rev=unverified
```

Vendor Result 0 is Ok; -4 is Timeout. A skipped GET prints `not-attempted`;
127 is a local sentinel for an unattempted SET, outside vendor Result values.
Failed GET logs `readback=unavailable getResult=-4`, without a match field.
No failure path claims a measured readback or matching configuration.

DSRV logging now adds source name, candidate meaning, existing routing context,
UA scan generation and suppressed-sample count. A global five-second bound
within the generation prevents alternating identities from flooding UART.
A new generation discards the old diagnostic window. DEBUG off retains no
pending sample; blocked/slow UART may skip arbitrary samples, counting them
with saturation. Only scalar state is retained, no packet or heap buffer.

Source 0 means data, 1 PAD, 2 DLS, 3 reserved. For sources 0/1, raw 2/7 is
labelled SLS-candidate/SPI-candidate. Source 2 is labelled DLS-raw even if the
raw field is nonzero; source 3 is reserved. Context is audio/sls/spi/unmatched
according to the existing router, or inactive when audio context is unusable.
Neither candidate names nor raw numbers change admission, presence or START/STOP.

## Evidence versus interpretation

The task supplied this **previous hardware excerpt**:

```text
[DAB/XPAD] value=0x0097 setResult=0 rev=unverified
[DAB/INV] services=2 components=2 audio=2 data=0 packetMOT=0 invalid=0 version=8
[DAB/UA] scan complete services=2 components=2 processed=2 failed=0
[DAB/SPI] confirmed=0 packetMOTUnverified=0 status=not signalled ...
[DAB/UA] ... type=0x002 (Slideshow) len=2
[DSRV/UA] raw16_17=0x0002 rev=unverified src=1 sid=00002FB9 cid=00000005
[SLS] Header TID=4790 length=21460 bytes=0
[SLS] Saved TID=4790 seg=0 len=2000 bytes=2000/21460
```

It demonstrates a successful SET acknowledgement and an observed raw 2 matching
the advertised slideshow application. It contains no GET_PROPERTY readback,
complete slideshow publication or SPI-positive on-air evidence. The negative
inventory/UA result is credible; diagnostics do not try to force EPG on.
The reported errors=1 is unresolved: save its command/result/reason during HW
regression rather than attributing it to EPG without evidence.

[Skyworks AN649 Rev.2.0](https://www.skyworksinc.com/-/media/Skyworks/SL/documents/public/application-notes/an649.pdf),
property 0xB400 p.239, documents the **used bits** in 0x0097 as:

| Bit | Rev.2.0 meaning |
| --- | --- |
| 0 | DLS_ENABLE |
| 1 | RFU |
| 2 | MOT_SLS_ENABLE |
| 4 | TPEG_ENABLE |
| 7 | EPG_ENABLE |

Unset bits: 3 BWS, 5 DGPS, 6 TMC, 8 DAB Java, 9 DMB, 10 IPDC,
11 Voice, 12 Middleware, 13 Journaline, 14 reserved, 15 other applications.
The DAB command 0x84 response in section 4.3 pp.176–177 calls bytes 16–17
UATYPE. The same revision's shared section 7.7 Table 19 p.426 still calls them
RFU. Local Rev.1.9 DAB table p.170/shared Table 19 p.400 also uses RFU; its
B400 map pp.224–225 defines DLS/MOT/TDC at 0/1/2 and reserves higher bits.
These differences prevent assigning the complete Rev.2.0 contract to FW6.0.9
solely from a single matching SLS value. **0x0097 stays unchanged.** An alternative
mask would require separate hardware/documentation evidence and approval.

Proven in host tests: ordered SET/GET behavior, error handling, diagnostic
pacing/context and preservation of existing routing/discovery. Interpreted from
documentation: UATYPE and flag names. Still unverified on the receiver: new
GET readback, complete UATYPE compatibility, and actual SPI/EPG reception.

## Validation and firmware

Passed strict C++11 host tests (`-Wall -Wextra -Werror -pedantic`): scheduler,
fm_features, dab_service_switch, dab_user_application, dab_spi_discovery,
dab_spi_probe, dab_packet_inventory, dab_dsrv_header, dab_diagnostics and
i2s_audio (production NVS with stubs).

Passed Python integration tests: test_dab_packet_runtime.py,
test_dab_xpad_integration.py and test_i2s_integration.py.
The XPAD test compiles the actual boot function and actual DSRV log block,
injecting read/write failures, timeout, mismatch, UART full, raw 2/7, foreign
identity, inactive context and generation changes. Source guards verify DAB-only
boot placement, early/reset invalidation, no live GET, generation rejection
before DSRV handling, and the unchanged spiPresent GUI binding. Runtime logger
tests verify success/mismatch/unavailable text and no false match. Fixtures are
synthetic; transport timing, RF, actual hardware reset and audio are not emulated.

Simulated output from the actual production diagnostic block:

```text
[DSRV/UA] raw16_17=0x0002 meaning=SLS-candidate src=1 source=PAD sid=00002FB9 cid=00000005 context=audio gen=1 rev=unverified suppressed=1
[DSRV/UA] raw16_17=0x0007 meaning=SPI-candidate src=1 source=PAD sid=00002FB9 cid=00000005 context=audio gen=1 rev=unverified suppressed=1
```

The 0x0007 line is **simulated**, not received from the air. Existing tests still
cover true synthetic UA0x007 confirmation, negative scans and unverified packet MOT.
Raw values cannot bypass those rules or the old-generation command guard.

`pio run -e esp32dev`: SUCCESS. RAM **62,272 B** (+16 from 62,256); flash
**3,458,193 B** (+1,044 from 3,457,149). MOT workspace stays **51,200 B**.
The existing TFT_eSPI TOUCH_CS warning remains. No new delay/busy-wait, runtime
heap buffer, menu, NVS, I2S or UI change was introduced. DSRV 24-byte header,
byte count/segment offsets, readCurrentReply calls, buffer choice, event drain,
legacy SLS/DLS processing and overflow accounting remain unchanged.

Application image: `.pio/build/esp32dev/firmware.bin`, flash address **0x10000**.
SHA256: `C4F60BE9E9B105E450C381F9188723231FD3387F9A8DD36DB232A8459B8ED242`.
The merged receiver-complete.bin at 0x0 replaces NVS settings; use the application
image to retain settings. No upload or physical receiver test was performed.

## Receiver checklist

1. Load the application image, tune the supplied negative mux and enable DEBUG
   once. Save the XPAD line: separate setResult/getResult, actual readback and
   match. Expect 0097/0/0/match=1 only if the chip really returns that value.
2. Confirm uninterrupted audio, dynamic text and completed/new JPEG or PNG SLS.
   Raw 0002 should remain diagnostic and show the proper audio/PAD context.
3. Confirm EPG remains grey, confirmed=0, packetMOTUnverified=0. Raw 0007 alone
   must not light it; a real positive SPI test belongs to a later task.
4. Switch multiplexes and DAB/FM/DAB, then test standby/recovery as usual.
   FM must not read B400; the next DAB initialization obtains a fresh record.
5. Compare CTS timeout/recovery, DSRV overflow and UART dropped/error counters;
   save the cause of any added error. Test DEBUG off/on and congestion: radio
   continues while diagnostics may skip lines. No readback polling should appear.

## Files changed

- src/si4684.cpp: boot-only readback, cache reset, paced log/context enrichment.
- src/si4684.h: compact records and boot function declaration.
- src/dab_diagnostics.h: new small diagnostic helpers, no router/selector logic.
- tests/test_dab_diagnostics.cpp: new helper/pacing/context regression fixtures.
- tests/test_dab_xpad_integration.py: new actual boot/log integration fixtures.
- tests/test_dab_packet_runtime.py: updated logger fixtures and routing guard.
- Serial_protocol.md and tests/README.md: current log format and test commands.
- tests/DAB_XPAD_UATYPE_diagnostics_report.md and .patch: report and review diff.

Vendor library and P/S decoding were not modified by this task.
