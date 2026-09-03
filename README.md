[![contributions welcome](https://img.shields.io/badge/contributions-welcome-brightgreen.svg?style=flat)](https://github.com/PE5PVB/TEF-Nextion-Multiband#contributing)
[![License](https://img.shields.io/badge/license%20-%20GNU_GPLv3-GPLv3?color=blue)](https://github.com/PE5PVB/TEF-Nextion-Multiband/blob/main/LICENSE)

# Note
The version in the repository is an ongoing development. It could and will contain bugs. To make sure you use the latest fully tested firmware, check the releases.

# SI4684 DAB/FM receiver
Advanced DAB/DAB+ and FM tuner software for the Skyworks SI4684 with an ESP32 board and a color LCD.  
More information: https://www.pe5pvb.nl/

## GPIO12: AUTO / INTB / IR

GPIO12 can be assigned in **Settings -> GPIO12**. The selected role is a boot-time configuration; changing it is saved together with all other Settings changes and the ESP32 restarts automatically when leaving Settings.

- **AUTO** - default/universal mode. The existing Si4684 INTB hardware detection is used. When the physical INTB connection is found, IRQ is used; otherwise the radio falls back to bounded polling. AUTO does **not** try to detect or initialize an IR receiver.
- **INTB** - forces the known Si4684 INTB path. The GPIO12 FALLING interrupt is attached at the start of radio initialization, before the normal Si4684 command sequence, and the existing 2 ms CTS safety polling remains enabled as a fallback.
- **IR** - reserves GPIO12 for a demodulated IR receiver. The Si4684 is controlled by polling and no radio interrupt handler is attached to GPIO12.

GPIO12 is the MTDI boot strap on the classic ESP32. A board that connects Si4684 INTB to GPIO12 must have VDD_SDIO safely fixed at 3.3 V by the hardware design or a correctly provisioned device. The firmware does not read, burn or modify eFuse.

The hardware variants are intentionally exclusive: connect GPIO12 to **Si4684 INTB, an IR receiver output, or neither**. Do not tie the Si4684 INTB output and an IR receiver output together.

Si4684 and ILI9341 continue to share the GPIO17 reset net.

## Transactional Settings storage

Settings are edited in RAM while the menu is open. When Settings is closed, the final values are compared with the values present when the menu was entered:

- unchanged values are not written;
- a value changed and then returned to its original setting is not written;
- all real changes are grouped together;
- if GPIO12 changed, all other changes made in the same Settings session (for example language, FM region and DAB/FM mode) are committed before the automatic restart.

When GPIO12 and DAB/FM mode are changed together, the firmware does not first perform a redundant live Si4684 mode reload. The requested radio mode is saved and the post-restart boot starts directly with the new GPIO12 role and radio mode.
Immediately before an automatic GPIO12-mode restart, the LCD backlight is switched off after the settings commit to hide the TFT reset/boot interval, matching the existing FM/DAB transition principle.

EEPROM writes were also removed from display-only routines such as tune-mode, memory-position and volume rendering. Those values are now persisted only when the user actually changes/selects them.
Restoring an already stored DAB service and switching to an unchanged stored FM frequency also avoid re-writing identical EEPROM data.

## IR remote learning

Settings contains **IR Remote** with the following functions. Learn and Test are active when the current boot is running with **GPIO12 = IR**; if IR has just been selected, leave Settings first so the automatic restart can initialize the receiver correctly.


- **Learn** - automatically learns one code for each supported radio action;
- **Clear** - removes the learned remote profile after confirmation;
- **Test** - shows the decoded protocol/address/command and mapped action without executing the action.

The learning sequence is:

1. TUNE +
2. TUNE -
3. OK
4. VOL +
5. VOL -
6. MODE
7. SLIDESHOW
8. STANDBY

The previous profile remains valid until all eight new keys have been learned successfully. Repeat frames are ignored during learning, each learned key must be released before the next step, and duplicate key assignments are rejected. The completed profile is written once and only if it differs from the stored profile.

During normal operation repeat is accepted for **TUNE +/-** and **VOL +/-**. Repeat frames for **OK, MODE, SLIDESHOW and STANDBY** are ignored.

Known IR protocols are stored as decoded protocol/address/command values. A hash decoder is enabled as a fallback for remotes that are not recognized by one of the enabled standard protocol decoders. Learned data uses the reserved EEPROM tail and does not move or change the existing DAB/FM preset layout. The EEPROM schema is version 5; upgrading from schema 4 preserves all existing settings and DAB/FM presets, initializes GPIO12 to AUTO and clears only the newly assigned IR-profile area.


## ESP32 static DRAM headroom

Arduino-IRremote adds a receive state/raw buffer to the classic ESP32 static DRAM.
To avoid exhausting the linker-visible `dram0_0_seg`, the firmware keeps the 99-entry FM preset cache and the learned IR working tables on the normal internal heap instead of `.bss`. The IR raw buffer remains at `RAW_BUFFER_LENGTH=100`, so protocol coverage is not reduced merely to save static RAM.

The FM cache allocation is about 1.4 kB and is performed once during boot before presets are loaded. In `GPIO12 = IR` mode both IR code tables are also allocated once during boot and retained for the firmware lifetime, so Learn/Test/Clear do not create repeated malloc/free cycles. On allocation failure the firmware reports the error instead of dereferencing a null pointer.

## Building instructions
On Youtube I published a video how to build your own radio.

In English:  
https://www.youtube.com/watch?v=C_xd0h_HTuU

In Dutch:  
https://www.youtube.com/watch?v=wV3G2J327qg

# Libraries
These are the libraries used for this project:

- https://github.com/Bodmer/TFT_eSPI
- https://github.com/Bodmer/JPEGDecoder
- https://github.com/bitbank2/PNGdec
- https://github.com/Arduino-IRremote/Arduino-IRremote - pinned to **4.7.1** by `platformio.ini`

The PlatformIO build resolves the declared dependencies automatically:

```text
pio run -e esp32dev
```

For the current PlatformIO build the settings in `platformio.ini` are authoritative. In particular `TFT_RST=-1` is intentional: GPIO17 is driven by the application because the TFT and Si4684 share that physical reset net.

Legacy Arduino-library settings were:

```cpp
#define ILI9341_DRIVER
#define TFT_CS          5
#define TFT_DC          4
#define TFT_RST         17
#define SPI_FREQUENCY   50000000
#define SMOOTH_FONT
```

# Buttons
A brief instruction for the physical controls:

- Top encoder: DAB channel/seek/preset; FM MAN +/-1.0 MHz, AUTO seek, or MEM preset.
- Bottom encoder: DAB service; FM regional raster step; press for headphones volume.
- Top button: short press Service Information, long press Stand-by mode.
- Middle button: short press Set mode, long press Open menu.
- Lower button: toggle Slideshow view.

## Contributing
I'm open for new ideas in our project. Feel free to share your thoughts in [Discussions](https://github.com/PE5PVB/SI4684-DAB-Receiver/discussions).  
You can also contribute your own code using [Pull Requests](https://github.com/PE5PVB/SI4684-DAB-Receiver/pulls). We will review it and merge into main branch.

If you use this software and find a difficulty, please create a new [issue](https://github.com/PE5PVB/SI4684-DAB-Receiver/issues) and describe the problem.

I also would like to invite you to join our Discord community where we share ideas and help each other with issues.  
[<img alt="Join the TEF6686 Discord community!" src="https://i.imgur.com/lI9Tuxf.png" height="120">](https://discord.gg/ZAVNdS74mC)

Special thanks to all [contributors](https://github.com/PE5PVB/TEF-Nextion-Multiband/graphs/contributors). You are awesome! ❤️

## License
This program is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation; either version 3 of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

## If you like this software
<a href="https://www.buymeacoffee.com/pe5pvb"><img src="https://img.buymeacoffee.com/button-api/?text=Buy me a coffee&emoji=☕&slug=pe5pvb&button_colour=FFDD00&font_colour=000000&font_family=Cookie&outline_colour=000000&coffee_colour=ffffff" /></a>


### Heap allocation policy

To keep the classic ESP32 linker DRAM segment below its limit, the FM preset
cache and the two IR learned-code tables are allocated from the normal internal
heap instead of permanent `.bss`. These allocations are performed once during
boot (the IR tables only when `GPIO12 = IR`) and are retained for the lifetime
of the firmware; the IR Learn wizard does not repeatedly allocate/free memory.
This does not change the EEPROM format or the number of presets.

### Verified build baseline

The preceding DRAM-fix revision was successfully linked by the user with PlatformIO
Espressif32 7.0.1 / Arduino-ESP32 3.20017.241212 and IRremote 4.7.1:

- RAM: 123,336 / 327,680 bytes (37.6%)
- Flash: 3,409,097 / 4,128,768 bytes (82.6%)

The no-fragmentation follow-up keeps the same static-DRAM strategy; it only changes
the lifetime of the IR heap buffers so they are allocated once at boot in IR mode.

### UI metadata and menu redraw behavior

Settings navigation updates only the selection rows while the visible menu
window stays on the same page. A row-window redraw is required only when the
11-item menu scrolls beyond its 9 visible rows.

Programme type (PTY) value 0 is treated as undefined/no programme type and is
left blank until a positive PTY value is available. Other unavailable DAB
metadata such as ECC/country flag, SID/EID, bitrate, protection level and
service type likewise remains blank until valid data is received.

### FM/DAB character encoding and radiotext

FM RDS and DAB charset `0000` text is converted from the complete EBU Latin
repertoire to UTF-8 before drawing. The conversion table follows ETSI TS 101 756
and is stored as numeric Unicode code points, so conversion does not depend on
the source editor's encoding. DAB registered label/Dynamic Label encodings
`0110` (UCS-2, big-endian) and `1111` (UTF-8) are also decoded.

DAB charset handling is kept per source: the ensemble label uses the charset
returned by DAB_GET_ENSEMBLE_INFO, every service-list row keeps its own AN649
SlCharset, and Dynamic Label (DLS) uses the charset carried in its own two-byte
Si468x DLS prefix. For `GET_DIGITAL_SERVICE_DATA` with `DATA_SRC = 2`, normal
Dynamic Label text is taken from `PAYLOAD+2`; its byte length is `BYTE_COUNT-2`.
This avoids both the rejected-DLS regression from attempting host-side X-PAD
segment reassembly and the older two-byte over-read beyond the DSRV payload.
Charset nibble `0x04` is normalized to the UCS-2 decoder path (`0x06`). The
remove-label command clears the current DLS.


During DAB boot/service restoration the cached EEPROM service name is not drawn
until the currently tuned multiplex/service list confirms the target service.
This prevents a stale station name from flashing briefly before the real label.

The radiotext strip is rendered identically in FM and DAB. Its 20-pixel source
and destination rectangles are both aligned to TFT y=220, and text width is
measured using FullLineSprite/FONT16 (the same renderer that draws the text).
This avoids the former one-pixel top/bottom remnants and font-dependent scrolling.

FM Program Service names use the same EBU-to-UTF-8 conversion on the main screen,
Service Information and the FM preset list. The FM stereo-blend indicator is
abbreviated as `ST xx%` on the main screen.
