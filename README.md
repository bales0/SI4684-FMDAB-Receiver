# SI4684 FM/DAB Receiver v2.0

Advanced FM, RDS/RBDS, DAB/DAB+ and DAB SlideShow receiver for the **Skyworks SI4684**, **ESP32-WROOM-32D** and a **320×240 ILI9341** display.

Firmware v2.0 uses separate SPI controllers for the TFT and radio, supports learned IR remote control, FM/DAB presets, multilingual UI, DAB MOT SlideShow, light-sleep standby and an optional serial control/diagnostic interface. It also introduces a new project-local universal **Si468x driver library** that centralizes the tuner command/CTS/error state machine and supports both bounded polling and real **INTB interrupt-driven handling**.

This project is based on the original open-source SI4684/DAB receiver work published by **PE5PVB** and has since been extensively reworked and extended for FM/RDS, the new Si468x driver, interrupt/poll operation, learned IR control, SlideShow, memory diagnostics and the current v2.0 architecture.

> [!CAUTION]
> ## GPIO12 / MTDI eFuse requirement — read before using INTB or IR
>
> On the classic ESP32, **GPIO12 is also the MTDI boot-strapping pin**. At reset it normally selects the VDD_SDIO flash voltage:
>
> - GPIO12 LOW or unconnected → **3.3 V**
> - GPIO12 HIGH → **1.8 V**
>
> **ESP32-WROOM-32D uses 3.3 V flash. For this project, program the VDD_SDIO eFuses to a fixed 3.3 V before using GPIO12 as either Si4684 INTB or IR receiver input.** This also applies to `GPIO12 = AUTO` if the INTB line is physically fitted.
>
> An Si4684 INTB output or a demodulated IR receiver normally idles HIGH. Without the eFuse override, a HIGH level present during reset can select 1.8 V for a 3.3 V flash device and the ESP32 may fail to boot.
>
> If GPIO12 is physically unconnected and the radio is used in polling mode, the eFuse change is not required. A hardware design that independently guarantees the correct VDD_SDIO voltage/strap can also solve the problem, but the standard WROOM-32D build in this project should treat the 3.3 V eFuse setting as mandatory for INTB or IR.
>
> **eFuse programming is permanent and cannot be undone. Verify that the ESP32 module really uses 3.3 V flash before burning it. Do not burn 3.3 V on a module that requires 1.8 V flash.**

Official Espressif references:

- [Set Flash Voltage — ESP32 / espefuse](https://docs.espressif.com/projects/esptool/en/latest/esp32/espefuse/set-flash-voltage-cmd.html)
- [Espressif FAQ — using GPIO12 on ESP32-WROOM-32D](https://docs.espressif.com/projects/esp-faq/en/latest/hardware-related/hardware-design.html)

---

## Main features

### DAB / DAB+

- DAB and DAB+ reception with SI4684.
- Uses the **Skyworks proprietary Si4684 DAB application firmware image** loaded into the tuner at runtime (`dab_6_0_9`, firmware 6.0.9 in the current build).
- Manual multiplex selection, automatic seek/scan and memory mode.
- Up to **99 DAB presets**.
- Ensemble and service list handling.
- Service selection from the channel/service list.
- Service ID (SID), Ensemble ID (EID), ECC/country information.
- Programme Type (PTY).
- Dynamic Label / DLS radiotext.
- Bitrate, sample rate, audio mode, service type and protection information when supplied by the tuner.
- DAB time/date information.
- DAB SlideShow / MOT reception.
- Automatic or manual SlideShow display.
- Character conversion for DAB labels/DLS including EBU Latin, UCS-2 and UTF-8 paths used by the firmware.

### FM

- FM reception using the **Skyworks proprietary Si4684 FM/FMHD application firmware image** loaded into the tuner at runtime (`fmhd_5_3_3`, firmware 5.3.3 in the current build).
- FM reception with regional band profiles:
  - **Europe:** 87.5–108.0 MHz, 100 kHz raster, 50 µs de-emphasis, RDS.
  - **North America:** 87.9–107.9 MHz, 200 kHz raster, 75 µs de-emphasis, RBDS.
  - **Japan:** 76.0–95.0 MHz, 100 kHz raster, 50 µs de-emphasis, RDS.
- Manual tuning, automatic seek and memory mode.
- Up to **99 FM presets**.
- RDS/RBDS:
  - PI
  - PTY
  - PS, assembled and displayed per received 2-character segment
  - RadioText (RT)
- Stereo/mono indication.
- Stereo blend indication.
- Multipath indication.
- RSSI and SNR display.
- Region-aware fine tuning and seek spacing.

### User interface

- 320×240 ILI9341 color TFT.
- Smooth fonts and sprite-based UI.
- Multiple color themes.
- Adjustable backlight.
- Signal units: `dBµV`, `dBf`, `dBm`.
- Service/channel list.
- System Information screen.
- Configurable inactivity time-out.
- Eight UI languages:
  - English
  - Dutch
  - Greek
  - German
  - French
  - Spanish
  - Polish
  - Romanian

### Other functions

- Learned IR remote control on GPIO12.
- IR Learn / Clear / Test menu.
- Light-sleep standby with state retained in RAM.
- Wake by physical STANDBY button.
- Optional wake by the learned IR STANDBY command when `GPIO12 = IR`.
- EEPROM-backed settings and presets with delayed/transactional writes.
- Serial control protocol at 115200 baud.
- Runtime `DEBUG` command for detailed FM/DAB/RDS/SLS/SPI/RAM diagnostics.
- Shared ILI9341/SI4684 reset recovery.
- Dedicated TFT VSPI and radio HSPI buses.

---

# Hardware

## ESP32

The project targets a classic **ESP32-WROOM-32D / ESP32 Dev Module**.

## TFT — ILI9341 / TFT_eSPI

| Signal | ESP32 GPIO |
|---|---:|
| SCLK | 18 |
| MISO | 19 |
| MOSI | 23 |
| CS | 5 |
| DC | 4 |
| RESET | 17, shared with SI4684 RSTB |
| Backlight PWM | 2 |

The TFT uses **VSPI/SPI3**. `TFT_RST=-1` is intentional: GPIO17 is controlled by the application because the TFT reset and Si4684 RSTB share the same physical net.

## SI4684

| Signal | ESP32 GPIO |
|---|---:|
| SCLK | 14 |
| MISO | 16 |
| MOSI | 13 |
| CS | 15 |
| RSTB | 17, shared with TFT RESET |
| INTB | 12 when fitted |

The Si4684 uses a dedicated **HSPI/SPI2** instance. The radio SPI bus is initialized once and is not re-created during normal FM/DAB operation.

Firmware v2.0 uses a new project-local universal **`Si468x` driver layer** (`vendor/si468x/Si468x.*`). It provides the common low-level transport and command state machine for the tuner instead of duplicating command handling in the FM and DAB paths. In particular it supports:

- normal bounded **polling** for CTS/status completion;
- **INTB interrupt-driven** command/event handling when the Si4684 INTB line is connected;
- polling as a safety/fallback path even when INTB is enabled;
- common command/reply parsing and device-error reporting;
- the same transport architecture for both FM and DAB firmware images.

The current build loads Skyworks-provided proprietary application images into the Si4684 with `LOAD_INIT` / `HOST_LOAD` / `BOOT` when changing the active radio firmware:

- DAB: `dab_6_0_9`
- FM/FMHD: `fmhd_5_3_3`

These tuner firmware images are **proprietary Skyworks material**, not GPL application source code. Their use and redistribution are subject to the applicable Skyworks terms/licence.

## Controls

| Control | ESP32 GPIO |
|---|---:|
| Upper rotary A | 27 |
| Upper rotary B | 34 |
| Upper rotary push / OK | 25 |
| Lower rotary A | 33 |
| Lower rotary B | 32 |
| Lower rotary push / Volume | 35 |
| MODE | 39 |
| STANDBY | 36 |
| SLIDESHOW | 26 |

GPIO34–39 are input-only pins and have no internal pull resistors; the PCB must provide the required external bias.

## Other hardware

- TPA6130A2 headphone amplifier over I²C.
- GPIO17 is a shared active-low hardware reset for both ILI9341 and SI4684.
- GPIO12 is mutually exclusive in the hardware design: connect it to **Si4684 INTB**, **IR receiver output**, or leave it unused. **Do not tie INTB and an IR receiver output together.**

---

# Mandatory eFuse setup for GPIO12 INTB / IR

## Why this is necessary

GPIO12 is not an ordinary GPIO during ESP32 reset. It is sampled as **MTDI** before the application starts and normally controls the internal VDD_SDIO regulator voltage.

For a normal ESP32-WROOM-32D:

| GPIO12 at reset | Default VDD_SDIO |
|---|---:|
| LOW / NC | 3.3 V |
| HIGH | 1.8 V |

Both of the project use cases below can place a HIGH level on GPIO12 before firmware gets control:

1. **Si4684 INTB connected to GPIO12** — includes explicit `INTB` mode and `AUTO` mode on hardware with INTB fitted.
2. **Demodulated IR receiver connected to GPIO12** — IR receiver outputs normally idle HIGH.

Therefore the recommended and expected configuration for the WROOM-32D build is to permanently force VDD_SDIO to **3.3 V** by eFuse.

## Short programming guide

Install a recent Espressif `esptool` package and identify the ESP32 serial port. The following example uses `COM5`; replace it with your actual port.

### 1. Read the current eFuse state

```text
espefuse --chip esp32 --port COM5 summary
```

Before programming, a factory device normally reports that flash voltage is determined by GPIO12/MTDI.

### 2. Verify the module

Confirm that the module/flash requires **3.3 V**. This README assumes **ESP32-WROOM-32D**.

### 3. Permanently set VDD_SDIO to 3.3 V

```text
espefuse --chip esp32 --port COM5 set-flash-voltage 3.3V
```

The tool displays an irreversible-operation warning and asks for confirmation. Read the proposed operation before confirming it.

Older esptool installations may use the legacy command form:

```text
espefuse.py --port COM5 set_flash_voltage 3.3V
```

### 4. Verify after programming

```text
espefuse --chip esp32 --port COM5 summary
```

A fixed 3.3 V configuration burns the ESP32 VDD_SDIO-related eFuses so GPIO12 is ignored for flash-voltage selection.

### 5. Verify on the receiver itself

Open **System Information** with a short press of the physical STANDBY button. The `VDD_SDIO` row reports both the boot-latched MTDI state and whether voltage selection comes from the strap or eFuse.

Typical examples:

```text
3.3V MTDI:H/EFUSE
3.3V MTDI:L/EFUSE
```

Both mean VDD_SDIO is forced to 3.3 V by eFuse and GPIO12 can safely be HIGH or LOW during reset on the WROOM-32D build.

```text
3.3V MTDI:L/STRAP
```

means the board booted with the normal strap selecting 3.3 V, but the eFuse override is **not** active.

```text
1.8V MTDI:H/STRAP
```

means GPIO12 was sampled HIGH and selected 1.8 V through the strap. This is **not the intended state for ESP32-WROOM-32D**.

> [!WARNING]
> eFuses are one-time programmable. The firmware never burns or changes them automatically. Changing `Settings -> GPIO12` does **not** program the eFuse.

---

# GPIO12 operating modes

Select the role in **Settings -> GPIO12**. Changing the GPIO12 role is a boot-time change: the setting is stored and the ESP32 restarts automatically when Settings is closed.

### AUTO

- Default/universal radio mode.
- The firmware tests for a real Si4684 INTB connection.
- If INTB is detected, radio IRQ operation is used.
- If INTB is not detected, the firmware falls back to bounded polling.
- AUTO never tries to detect an IR receiver.
- If the INTB wire is physically fitted to GPIO12, the 3.3 V eFuse requirement still applies.

### INTB

- Forces GPIO12 to be used as Si4684 INTB.
- FALLING-edge IRQ is installed before normal tuner commands start.
- CTS safety polling remains available as a fallback.
- Requires the GPIO12 / VDD_SDIO boot issue to be solved; for WROOM-32D this README expects the 3.3 V eFuse setting.

### IR

- GPIO12 belongs exclusively to the IR receiver.
- Si4684 runs in polling mode.
- No Si4684 GPIO12 IRQ handler is attached.
- IR Learn/Test functions become active after the restart into IR mode.
- Requires the GPIO12 / VDD_SDIO boot issue to be solved; for WROOM-32D this README expects the 3.3 V eFuse setting.

---

# Physical controls

The radio has two rotary encoders plus MODE, STANDBY and SLIDESHOW buttons.

## Upper rotary encoder — tuning / menu / OK

### Rotate in the main screen

Behavior depends on the selected tune mode:

| Tune mode | DAB | FM |
|---|---|---|
| **MAN** | previous/next DAB channel block | coarse tuning ±1.0 MHz |
| **AUTO** | start seek/scan down/up | start FM seek down/up |
| **MEM** | previous/next stored preset | previous/next stored preset |

In MEM mode, empty preset positions are skipped during normal recall.

### Rotate in Channel List

Moves the highlighted service/station. The selected row is activated with the upper rotary push/OK.

### Rotate in Settings

Moves through menu rows. After a menu item is opened for editing, rotation changes its value.

### Push / OK

- **Main screen, MAN/AUTO:** opens Channel List.
- **Channel List:** starts/confirms the highlighted service/station and returns to the main screen.
- **MEM mode:** first press arms preset storage; second press stores the currently tuned station/service in the selected memory position.
- **Settings:** opens the highlighted setting; pressing again closes the edit popup/sub-item.
- **Slideshow/System Info:** returns to the main display where applicable.

## Lower rotary encoder — service / fine tune / volume

### Rotate without volume overlay

- **DAB:** previous/next service in the current multiplex.
- **FM:** previous/next channel using the selected region raster:
  - Europe/Japan: 100 kHz
  - North America: 200 kHz
- No hidden tuning action is performed while System Information is open.

### Push

Opens the volume overlay.

### Rotate while volume overlay is open

Changes headphone volume. The overlay closes automatically after inactivity or can be closed by pressing the lower encoder again.

## MODE button

### Short press

- Main screen: cycles **MAN -> AUTO -> MEM -> MAN**.
- Full-screen sub-view: returns to the main screen.
- Settings: closes Settings and commits actual changed values.

### Long press — more than 1 second

- Main screen: opens **Settings**.

## STANDBY button

### Short press

Toggles **System Information**.

### Long press — more than 1 second

Enters light-sleep standby.

### While preset storage is armed

If MEM storage has been armed with the upper rotary push, pressing STANDBY deletes/clears the selected preset instead of opening System Information or entering standby.

## SLIDESHOW button

DAB only:

- If a complete SlideShow image is available, opens it immediately.
- If a MOT object is currently being received, opens a waiting/progress view and displays the image when reception completes.
- Press again to leave SlideShow.
- No action in FM mode.

---

# Tune modes and presets

## MAN

Direct manual tuning.

- DAB upper rotary: steps through DAB channel blocks.
- FM upper rotary: ±1.0 MHz coarse step.
- FM lower rotary: exact regional channel raster.

## AUTO

Starts seek/scan in the rotation direction.

- Repeating/holding IR TUNE does not continually restart an already running AUTO seek.

## MEM

Recall/store mode for 99 presets.

### Recall

Rotate the upper encoder. Empty positions are skipped and the stored DAB service or FM frequency is tuned.

### Store

1. Select `MEM` mode.
2. Press the upper rotary once to arm preset storage.
3. Rotate to the desired memory position if needed.
4. Press the upper rotary again to save the current service/station.

FM presets store frequency, PI and PS label when available. DAB presets store channel, service ID and service label.

### Delete

1. Select/arm the preset position as above.
2. Press STANDBY while preset storage is armed.
3. The selected preset is cleared.

---

# IR remote control

IR support is available only when **Settings -> GPIO12 -> IR** is selected and the radio has restarted.

A demodulated IR receiver output is connected to GPIO12. GPIO12 is then owned by the IR decoder and the Si4684 runs in polling mode.

> [!CAUTION]
> An IR receiver normally idles HIGH. On ESP32-WROOM-32D, program VDD_SDIO to fixed 3.3 V by eFuse before using the IR input on GPIO12. See the mandatory eFuse section above.

The runtime receiver uses a project-local GPIO edge-capture and state-machine implementation, so the full Arduino-IRremote library is not required as a build dependency. However, the supported protocol IDs, timing constants and decoder behaviour in `ir_decode.cpp` are **derived from the public Arduino-IRremote 4.7.1 decoder sources**. Those derived portions are used under the **MIT License** and retain the corresponding copyright/licence notice in the source file.

## Learning a remote

Open:

**Settings -> IR Remote -> Learn**

The wizard learns these eight actions in order:

1. `TUNE +`
2. `TUNE -`
3. `OK`
4. `VOL +`
5. `VOL -`
6. `MODE`
7. `SLIDESHOW`
8. `STANDBY`

Learning behavior:

- The previous profile remains active until all eight new keys are learned successfully.
- Repeat frames are ignored while learning.
- Each key must be released before the next step.
- Duplicate assignments are rejected.
- The completed profile is written only when it differs from the stored profile.

## IR Remote menu

### Learn

Starts the eight-key learning wizard.

### Clear

Deletes the learned profile after confirmation.

### Test

Shows decoded protocol/address/command and the mapped action without executing the radio action.

## IR actions during normal operation

| Learned key | Short press | Hold / long press | Repeat behavior |
|---|---|---|---|
| **TUNE +** | DAB: normal up action; FM MAN: one regional raster step up; AUTO/MEM: normal up action | repeats after hold delay except AUTO seek | auto-repeat after about 500 ms; AUTO repeats are ignored so an active seek is not restarted |
| **TUNE -** | DAB: normal down action; FM MAN: one regional raster step down; AUTO/MEM: normal down action | repeats after hold delay except AUTO seek | auto-repeat after about 500 ms; AUTO repeats are ignored so an active seek is not restarted |
| **OK** | same as upper rotary push | no separate long action | repeats ignored |
| **VOL +** | volume +2 | continues increasing | auto-repeat after about 500 ms |
| **VOL -** | volume -2 | continues decreasing | auto-repeat after about 500 ms |
| **MODE** | cycle MAN/AUTO/MEM after key release | **hold >= 1 s: switch DAB <-> FM once** | handled as one short-or-long action, not repeated |
| **SLIDESHOW** | toggle DAB SlideShow | no separate long action | repeats ignored |
| **STANDBY** | enter standby; **when waking from light sleep use a double-click (two presses)** | no separate long action | repeats ignored during normal operation; wake qualification uses the second complete learned STANDBY frame |

### IR TUNE +/- behavior

IR TUNE follows the radio context, with one deliberate FM difference from the physical upper encoder: in **FM MAN** it uses the selected region's exact channel raster (100 kHz Europe/Japan, 200 kHz North America), so every valid station remains reachable from a remote that has only one TUNE +/- pair. In AUTO mode the first press starts seek and held-key repeats are ignored until a new press.

### IR MODE short press

A short learned MODE press is deliberately executed **on release**. It performs the same normal mode action as the physical MODE short press: MAN/AUTO/MEM cycling or contextual return behavior.

### IR MODE long press

Hold the learned MODE key continuously for at least **1 second**:

```text
DAB <-> FM
```

The long action is fired only once. Releasing the key does not then generate the short MODE action.

## IR WAKE — double-click is required

When waking the receiver from light sleep with the learned IR **STANDBY** key, use a **double-click: press STANDBY twice in quick succession**. This is the documented and required IR wake gesture for this firmware.

Why two presses are used:

1. **First STANDBY press** — generates the GPIO12 edge that wakes the ESP32 from light sleep. Because the CPU starts in the middle of that transmission, the beginning of the IR frame may be missing and it is not accepted as the wake command.
2. **Second STANDBY press** — must arrive within the wake qualification window (up to about **1.5 seconds**) and supplies a complete learned STANDBY frame. Only after this frame is decoded and matched does the firmware restore the display/audio and complete wake-up.

So the expected user action is:

```text
IR STANDBY:  press -> press   (double-click)
```

This is intentionally **not** a general double-click function for other IR keys during normal operation. It is specific to IR wake from light sleep and prevents random IR traffic, noise, or another learned button from turning the receiver on.

The physical STANDBY button on GPIO36 wakes the receiver immediately and does **not** require a double-click.

---

# Settings

Open Settings with a **long physical MODE press (>1 s)**.

Use the upper rotary to navigate, press the upper rotary to edit/select, rotate to change values, and press again to leave the edit item. Press MODE to leave Settings.

The menu contains:

1. **Language**
2. **Brightness**
3. **Theme**
4. **Auto slideshow**
5. **Signal unit** — dBµV / dBf / dBm
6. **Time-out Timer** — Off / 15 / 30 / 60 / 90 minutes
7. **Radio mode** — DAB / FM
8. **FM Region** — Europe / North America / Japan
9. **GPIO12** — AUTO / INTB / IR
10. **IR Remote** — Learn / Clear / Test
11. **About**

Settings are edited in RAM and committed when leaving Settings. Values that did not actually change are not unnecessarily rewritten to EEPROM.

Changing GPIO12 causes an automatic restart after the settings have been committed, because GPIO12 ownership is established during boot.

---

# Boot-time button shortcuts

The following physical keys are sampled during power-up:

| Keys held during boot | Function |
|---|---|
| **SLIDESHOW only** | invert rotary direction and save it |
| **MODE** | rotate/flip the display orientation by 180° and save it |
| **SLIDESHOW + upper rotary push/OK** | factory defaults / clear settings and presets, then restart |

Release the key(s) when the on-screen instruction asks for it.

---

# System Information

Open with a short physical STANDBY press.

The page includes:

- Si4684 tuner type and firmware version.
- Active radio mode.
- Radio control mode: INTB / polling / detection state.
- GPIO12 configured/runtime role and live electrical level.
- VDD_SDIO status including boot-latched MTDI and STRAP/EFUSE source.
- ESP32 revision, CPU frequency and firmware version.
- Uptime.
- Reset reason.
- Free/minimum heap.
- Largest heap block and calculated fragmentation.

The VDD_SDIO row is especially useful after the GPIO12 eFuse programming described above.

---

# DAB SlideShow / MOT

The firmware implements a RAM-only DAB MOT SlideShow path intended for the DAB SlideShow Simple Profile used by this receiver.

- Maximum MOT image payload buffer: **51,200 bytes (50 KiB)**.
- Supported display size: up to **320×240**.
- Static PNG support.
- Baseline JPEG support used by the DAB Simple Profile.
- Progressive JPEG is not required by the Simple Profile and is not part of the guaranteed receiver profile.
- Shared early decoder workspace: **76,800 bytes**.
- No late JPEG/PNG fallback `malloc/calloc` allocation during normal rendering.
- Incoming MOT segments may arrive out of order; the collector tracks received segments and assembles only a complete object.
- A partial old Transport ID is abandoned when a new object begins.

### Manual SlideShow

Press SLIDESHOW or the learned IR SLIDESHOW key.

### Automatic SlideShow

Enable **Settings -> Auto slideshow**. A completed image can automatically enter the SlideShow view.

---

# Standby / light sleep

A long physical STANDBY press, or the learned IR STANDBY action, enters ESP32 light sleep.

During light sleep:

- CPU execution pauses.
- RAM is retained.
- Si4684 firmware/service state remains resident.
- MOT/decoder workspaces remain allocated.
- ILI9341 enters Sleep In.
- Backlight is off.
- TPA6130A2 is shut down.
- GPIO17 shared reset is held HIGH.
- VDD_SDIO is explicitly kept powered so GPIO16/17 state is retained correctly.

Wake sources:

- Physical STANDBY GPIO36 LOW — always available and wakes immediately.
- GPIO12 IR wake — available only when `GPIO12 = IR` **and** a learned IR profile exists. **IR wake requires a double-click of the learned STANDBY key:** the first press wakes the ESP32, the second complete frame within about 1.5 s confirms the learned STANDBY command.
- GPIO12 is not used as a sleep wake source in AUTO or INTB mode.

After wake, the TFT uses the ILI9341 wake sequence rather than an unnecessary software reset, then the main UI is redrawn before the backlight is restored.

---

# Serial interface and DEBUG diagnostics

UART speed:

```text
115200 baud
```

Normal application diagnostics are intentionally **OFF** so the UART is not continuously flooded by FM/DAB polling, RDS and MOT progress messages.

Send the bare command:

```text
DEBUG
```

The receiver replies:

```text
[DEBUG] diagnostics ON
```

Detailed diagnostics then become available, including:

- boot/RAM information;
- FM status;
- DAB status;
- RDS PI/PTY/PS/RT processing;
- MOT headers and segment progress;
- SlideShow JPEG/PNG status;
- SI4684 SPI replies and errors;
- firmware HOST_LOAD progress;
- IR frames and sleep/wake qualification;
- memory-integrity reports.

Send `DEBUG` again to disable them:

```text
[DEBUG] diagnostics OFF
```

The command parser accepts CR, LF, CRLF and also a command sent with no line ending after a short idle timeout.

Critical startup/fatal errors remain visible even when DEBUG is off.

The external serial control protocol remains independent of the diagnostic gate. Main control commands include `ENABLE`, `TUNE`, `SERVICE` and `INTERVAL`; see `Serial_protocol.pdf` / `Serial_protocol.docx` in the repository for the complete protocol description.

---

# EEPROM and stored data

Current EEPROM schema: **5**.

Stored information includes:

- UI/settings values.
- Current DAB/FM mode.
- DAB restore frequency/service ID.
- FM frequency/region.
- 99 DAB presets.
- 99 FM presets.
- GPIO12 role.
- Learned eight-key IR profile.

Known older schemas are migrated without moving the established DAB preset layout. EEPROM writes are delayed/grouped so display refreshes do not repeatedly write flash-backed EEPROM storage.

---

# SPI and reset architecture

The ESP32 uses two independent SPI hosts:

```text
ILI9341  -> VSPI / SPI3
SI4684   -> HSPI / SPI2
```

This prevents both devices from trying to initialize/own the same SPI host.

GPIO17 is physically shared by TFT RESET and Si4684 RSTB. The application therefore follows these rules:

- `TFT_RST=-1` in TFT_eSPI.
- GPIO17 is driven manually.
- Cold boot performs one controlled shared hardware reset.
- Normal light-sleep wake does not reset the tuner or reinitialize the SPI hosts.
- FM/DAB firmware-image changes use the controlled shared-reset recovery path while the backlight is hidden.

---

# Memory strategy

Large lifetime buffers are allocated early from internal RAM, before TFT sprites/fonts can fragment the heap:

- DAB MOT buffer: 51,200 bytes.
- JPEG/PNG shared decoder workspace: 76,800 bytes.
- Si4684 HOST_LOAD workspace: 4,096 bytes.

These blocks remain allocated for the firmware lifetime. Small fixed runtime state such as FM preset cache and IR learned/work tables is kept in fixed storage. Runtime rendering avoids large fallback heap allocations.

System Information and DEBUG diagnostics can be used to watch free heap, minimum heap and largest allocatable block.

---

# Building with PlatformIO

The current `platformio.ini` is the authoritative build configuration.

```text
pio run -e esp32dev
```

Important build settings include:

- board: `esp32dev`
- framework: Arduino
- monitor: 115200 baud
- custom `partitions.csv`
- `CORE_DEBUG_LEVEL=1`
- ILI9341/TFT_eSPI pin setup passed through build flags
- TFT SPI frequency: 27 MHz
- `TFT_RST=-1`
- smooth fonts enabled

Current library dependencies declared by PlatformIO:

- `paulstoffregen/Time`
- `bodmer/TFT_eSPI`
- `bitbank2/PNGdec`

The IR receiver/capture runtime is implemented locally and therefore does not require Arduino-IRremote as a PlatformIO dependency. The protocol decoders/timing behaviour in `ir_decode.cpp` are derived from **Arduino-IRremote 4.7.1** and used under its MIT licence terms.

---

# Software architecture, firmware and acknowledgements

## New universal Si468x driver

Firmware v2.0 replaces the older duplicated radio-transport approach with a new project-local universal **Si468x** library/driver layer. The same driver handles low-level Si468x command transport for both FM and DAB operation and supports two radio-control methods:

- **POLL** — bounded CTS/status polling;
- **INTB** — hardware interrupt/event handling on GPIO12, with polling retained as a safety fallback.

`AUTO` mode can detect a usable INTB connection and select interrupt handling automatically; otherwise the driver continues in polling mode. This makes the radio code usable on boards both with and without the INTB connection.

## Skyworks Si4684 firmware images

The ESP32 application does not implement the DAB or FM demodulator internally. The Si4684 requires its vendor application firmware to be uploaded after reset. The current project contains/uses the Skyworks application images required by the tuner:

- **DAB 6.0.9** (`dab_6_0_9`)
- **FM/FMHD 5.3.3** (`fmhd_5_3_3`)

They are transferred to the Si4684 using the tuner `LOAD_INIT`, `HOST_LOAD` and `BOOT` sequence. These images are **proprietary Skyworks firmware**, are not made open-source by this project, and are not covered by the GPL licence that applies to the receiver application source. Users/distributors are responsible for complying with the applicable Skyworks terms.

## IR decoder origin

The IR subsystem uses its own ESP32 GPIO edge capture, timing collection, learning, wake qualification and runtime action state machines. For protocol decoding, `ir_decode.cpp` uses protocol definitions, timings and decoder behaviour derived from the public **Arduino-IRremote 4.7.1** sources. The relevant derived code is used under the **MIT License**, with attribution/licence text retained in the source. This gives the project broad remote-protocol compatibility without requiring the complete Arduino-IRremote runtime/library dependency.

## Original open-source project

This receiver is **based on the original open-source SI4684/DAB receiver project by PE5PVB**. The original project provided the foundation for the hardware/software concept and earlier receiver implementation. The current v2.0 branch extends and restructures that work substantially, including FM/RDS/RBDS, the new universal Si468x driver, polling/INTB operation, learned IR remote control and wake, memory/SPI redesign, SlideShow handling and the current diagnostics/system-information architecture.

Original project information and build material remain linked in the resources section below.

---

# First-time setup checklist

1. Assemble the board and verify all supply voltages.
2. Confirm the ESP32 module type and flash voltage.
3. **If GPIO12 will be connected to Si4684 INTB or an IR receiver, burn the ESP32-WROOM-32D VDD_SDIO eFuse to fixed 3.3 V before normal use.**
4. Verify the burn with `espefuse ... summary`.
5. Build and flash firmware v2.0.
6. Start with `GPIO12 = AUTO` if no IR receiver is fitted.
7. For a known INTB-wired board, select `GPIO12 = INTB` if desired.
8. For IR hardware, select `GPIO12 = IR`, leave Settings and allow the automatic restart.
9. Open `Settings -> IR Remote -> Learn` and learn all eight keys.
10. For IR standby wake, remember that **WAKE uses a double-click of the learned STANDBY key**: first press wakes the CPU, second press confirms the full learned frame.
11. Check System Information, especially `GPIO12` and `VDD_SDIO`.
12. Send `DEBUG` over UART only when detailed diagnostics are needed.

---

# Troubleshooting

## ESP32 does not boot after connecting INTB or IR to GPIO12

Disconnect the GPIO12 peripheral and check whether the board boots again. If it does, verify the VDD_SDIO/eFuse configuration. On WROOM-32D a HIGH GPIO12 strap can select 1.8 V unless the flash voltage has been forced to 3.3 V.

## System Information shows `.../STRAP`

The VDD_SDIO voltage is still being selected by the boot strap. For the WROOM-32D build with INTB or IR on GPIO12, program and verify the fixed 3.3 V eFuse setting.

## IR menu Learn/Test is inactive

Select `Settings -> GPIO12 -> IR`, exit Settings, and allow the automatic restart. IR capture is initialized only after booting in explicit IR mode.

## IR does not wake the radio

- A valid learned profile must exist.
- GPIO12 must be configured as IR.
- The IR receiver must return to idle HIGH before sleep is armed.
- **Use a double-click of the learned STANDBY key.** The first press wakes the ESP32; the second complete learned frame must arrive within about 1.5 s to confirm wake.

## Serial monitor is quiet

This is normal. Send:

```text
DEBUG
```

to enable diagnostics.

## SlideShow is not displayed

A complete MOT object must be received. With DEBUG enabled, inspect SLS/MOT headers, segments, total length and decoder result.

---

# Build information and project resources

The repository contains:

- source code under `src/`;
- KiCad/schematic resources;
- enclosure STL files;
- `Serial_protocol.pdf` and `Serial_protocol.docx`;
- `partitions.csv`;
- GPL license information.

Original project/build information and videos:

- This project is derived from the original open-source **PE5PVB SI4684/DAB receiver project**.
- English build video: https://www.youtube.com/watch?v=C_xd0h_HTuU
- Dutch build video: https://www.youtube.com/watch?v=wV3G2J327qg
- PE5PVB project information: https://www.pe5pvb.nl/
- IR protocol decoder basis: Arduino-IRremote 4.7.1 (MIT-licensed decoder sources; attribution retained in `ir_decode.cpp`).
- Si4684 radio application firmware: proprietary Skyworks DAB/FM firmware images used by the tuner.

---

# License

This project is distributed under the **GNU General Public License v3**. See `LICENSE` for the full license text.

Contributions, bug reports and improvements are welcome through the project repository.
