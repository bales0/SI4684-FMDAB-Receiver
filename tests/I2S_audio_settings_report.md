# Configurable Si4684 I²S audio output

Implemented from `Specification/Codex_task_add_I2S_audio_settings_sample_rate_off.md`.
The user approved the implementation batch before source changes. No device
was flashed and no receiver measurements were performed.

## Behavior and API

Settings now contains **I²S Audio** immediately after GPIO12 and before IR.
The four values are OFF, 32 kHz, 44.1 kHz and 48 kHz. Both encoder
directions wrap. The existing nine-row window, popup rendering and row height
remain in use. There are 14 logical rows; named indices keep IR at 10,
FM Seek at 11, FM AF at 12 and read-only About at 13. The label is available
in all eight existing languages; the project currently has no Czech locale.

`I2sAudioMode` is one byte. `sanitizeI2sAudioMode`, `i2sAudioSampleRate`,
`i2sAudioModeText`, `cycleI2sAudioMode` and `i2sAudioPinConfig` centralize the
mapping. The output rate is independent of received audio metadata.

`DAB::setI2sAudioMode(I2sAudioMode)` selects the configuration for the next
application initialization. Editing the popup only changes the requested
value. Closing Settings compares the final selection with the entry snapshot;
returning to the original selection does not reload the radio. A changed
selection is committed before calling the driver setter and the existing
`SwitchRadioMode(requestedRadioMode, true)` path. This resets/reloads Si4684
and restores the shared TFT controller without restarting ESP32. Same-band
DAB reload retains the live SID/CID rather than falling back to a possibly
older saved SID; FM uses the existing current-frequency restore.

A simultaneous GPIO12 change retains its existing ESP32 restart behavior,
after the I²S key has been committed. A commit failure cancels reinitialization,
shows the existing NVS error style and retains dirty data for the existing
storage retry mechanism. A radio initialization failure follows the existing
bounded failure path and does not start a new retry loop.

## Persistence and migration

The subsequent user-approved revision removes 40 kHz and renumbers the enum
without preserving the earlier I²S values: 0=OFF, 1=32 kHz, 2=44.1 kHz,
3=48 kHz. Earlier stored 2 and 3 are interpreted directly with these new
meanings; earlier stored 4 is invalid and is repaired to the new default 3.
No migration layer is added. Other settings and records retain their layout.

The optional unsigned-byte key is `si4684/i2s_audio`. There is no extension
to the legacy `settings` record, CONFIG_LENGTH, address map, schema marker,
preset blobs, IR profile or scan records. Missing and invalid values select
48 kHz. A missing optional key needs no write at boot; an invalid stored enum
is repaired to 48 kHz. Editing saves the key through the normal dirty/commit
path. Factory defaults explicitly save 48 kHz, including after an earlier OFF
selection. All four values were persisted and reloaded in the host test.

## Si4684 properties, pins and recovery

The MASTER field is bit 15 of DIGITAL_IO_OUTPUT_SELECT, as specified by
[Skyworks AN649](https://www.skyworksinc.com/-/media/Skyworks/SL/documents/public/application-notes/an649.pdf).
For enabled output, every `DAB::begin` writes `0x0200 = 0x8000` followed by
`0x0201 = 32000 / 44100 / 48000` before enabling the output pins and
before tuning. OFF skips these digital property writes. The existing
`0x0202 = 0x1600` format write is retained.

PIN_CONFIG_ENABLE (`0x0800`) is composed independently:

| Output state | Polling / GPIO12 IR | INTB enabled |
| --- | --- | --- |
| OFF, or digital configuration failed | `0x0001` | `0x8001` |
| I²S enabled and configured | `0x0003` | `0x8003` |

Bit 0 keeps the analog DAC enabled. Bit 1 enables I²S only after successful
digital configuration. Bit 15 belongs to the existing INTB decision. Both
failed AUTO-probe fallback and runtime polling fallback use the same pin
composition and preserve the selected audio state. Failure of either digital
property logs its address, requested value and result, leaves the saved mode
unchanged and attempts to continue with analog-only pins. Failure of the pin
configuration itself still fails initialization through the existing path.

The common `DAB::begin` path covers cold boot, DAB/FM application changes,
forced same-band initialization and the existing recovery flow. The output
ready flag is reset on every begin and configuration attempt, so a previous
successful boot cannot enable digital pins after a failed later attempt.
The ESP32 remains control/UI only; no audio buffers, DMA or resampling were added.

## Exact files changed for this task

This list excludes changes already present from earlier tasks.

- `SI4684-DAB-Receiver.ino`: boot selection, snapshot/apply, factory default,
  forced reload and current DAB SID/CID restoration.
- `src/i2s_audio.h` (new): enum and mode/rate/text/cycle/pin helpers.
- `src/settings_menu.h` (new): named indices for the inserted and following rows.
- `src/gui.cpp`: new row/editor and adjusted IR/FM/About dispatch.
- `src/gui.h`: menu constants and requested/active setting declarations.
- `src/language.h`: eight I²S labels; the pre-existing version edit is retained.
- `src/nvs_storage.h`, `src/nvs_storage.cpp`: separate optional key and dirty state.
- `src/si4684.h`, `src/si4684.cpp`: deferred-setting API, per-boot configuration
  and pin composition in initialization and both fallback paths.
- `tests/test_i2s_audio.cpp` (new): mapping, cycling, pins, indices and production NVS tests.
- `tests/test_i2s_integration.py` (new): actual configuration with injected errors,
  initialization/UI wiring checks and existing font checks.
- `tests/stubs/nvs/Arduino.h`, `tests/stubs/nvs/Preferences.h` (new): host NVS fixture.
- `tests/README.md`: reproducible commands and report link.
- `tests/I2S_audio_settings_report.md` (new): this report and hardware checklist.

## Validation results

- `pio run -e esp32dev`: PASS. Existing TFT_eSPI warning about unconfigured
  TOUCH_CS remains; no new compiler errors.
- Static RAM: **62,232 bytes**, unchanged from the preceding firmware.
- Flash: **3,455,849 bytes**, +1,552 bytes from the preceding firmware.
- Strict C++11 host mode/NVS test: PASS. Covers all 256 enum inputs, exact rates,
  both INTB states, both cycle directions, legacy blob preservation, reboot
  persistence of every mode, invalid-key repair, failed commit/retry and reset
  to 48 kHz.
- Actual configuration block with mock transport: PASS for OFF, every enabled
  rate, repeated initialization, SELECT failure and SAMPLE_RATE failure. Checks
  write order, requested value retention, analog pins and INTB independence.
- Menu/initialization/fallback wiring and font glyph/width checks: PASS.
- Existing scheduler, FM features, DAB service switch, user application,
  SPI discovery and SPI probe/router host suites: PASS.
- `git diff --check`: PASS.

Firmware: `.pio/build/esp32dev/firmware.bin`, application address **0x10000**.
SHA256: `DF5B48B8028E0C6D0A29733479AFDDAF62E69DB43D2150B3518240B166F8AAC1`.
The build also generates a complete image; that image replaces NVS. Use the
application-only image when checking upgrade persistence on an existing receiver.

## Expected serial diagnostics

Send the bare `DEBUG` command to enable diagnostics. These are expected examples,
not captured hardware output. To observe initialization lines, enable DEBUG and
then change the setting or switch bands. `result=0` means vendor-driver success.

```text
[I2S] mode=44.1 kHz enabled=1 master=1 rate=44100
[I2S] property=0x0200 value=0x8000 result=0
[I2S] property=0x0201 value=0xAC44 result=0
[RADIO/IRQ] runtime PIN_CONFIG=0x8003 result=0
```

The last line becomes `0x0003` with polling/IR. OFF with INTB:

```text
[I2S] mode=OFF enabled=0 master=0 rate=0
[RADIO/IRQ] runtime PIN_CONFIG=0x8001 result=0
```

The OFF line reports that master configuration was skipped; the pin bit disables
the digital output. Failed digital configuration prints the failing property's
actual result code, followed by:

```text
[I2S] configuration failed; digital pins disabled, analog DAC retained
```

## Hardware checklist — pending

1. Upgrade using the application image without erasing NVS. Confirm existing
   settings, DAB/FM presets and IR profile survive. With no I²S key, confirm
   48 kHz; an existing key follows the direct enum mapping described above.
2. Scroll through all 14 Settings rows. Cycle I²S both ways through all four
   values. Check popup text, IR learning, FM Seek/AF and read-only About.
3. Turn through several rates within the popup: no radio reload until leaving
   Settings. Return to the original value: no reload. Change the final value:
   one existing radio reset/reload, with current DAB service or FM frequency restored.
4. For each rate in DAB and FM, verify analog/headphone audio and measure I²S
   LRCLK at exactly 32,000 / 44,100 / 48,000 Hz. Confirm Si4684 supplies
   clocks and the external DAC plays correctly with the retained data format.
5. Select OFF in both bands. Verify analog audio continues and digital output
   is disabled. Confirm persistence after complete power removal.
6. Repeat band changes and available recovery/forced reload paths for OFF and
   an enabled rate. Confirm the selected output returns without ESP32 restarting
   solely for I²S and that no old audio service returns after a same-band reload.
7. Check GPIO12 AUTO with a working INTB, AUTO with missing INTB/fallback, forced
   INTB and IR for both OFF and enabled output. Check INTB behavior, analog output
   and digital enable bits. Change GPIO12 and I²S together and verify both survive
   the existing restart.
8. Verify factory reset restores 48 kHz. If a transport error can be reproduced
   safely, check exact-property error logging, analog-only fallback where the
   transport remains usable and absence of repeated recovery loops.
9. Check DAB infotext, SLS loading/manual display, EPG indication, live RSSI,
   volume/loading overlays and rapid Rotary1/Rotary2 service selection. Check
   FM audio, tuning, RDS, AF, volume and mute behavior.

Host tests establish software behavior; physical clocks, analog audio and
receiver coexistence remain to be verified on hardware.
