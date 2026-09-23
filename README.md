# TUNA UI

Touch UI for the TUNA instrument (Cambrian Bioworks), running on the
**Waveshare ESP32-S3-Touch-LCD-3.5B** (AXS15231B QSPI panel + touch, 480x320 landscape).
It talks to a Raspberry Pi bridge over serial, and the Pi forwards commands to Klipper/Moonraker.

Plain Arduino (no PlatformIO). Everything needed to build is in this folder.

## Layout
```
tuna_ui/                        the sketch (Arduino requires folder name == .ino name)
  tuna_ui.ino                   UI, state machine, serial protocol
  esp_lcd_touch_axs15231b.*     touch driver (I2C 0x3B)
libraries/                      exact library versions this project is built against
  lv_conf.h                     LVGL config (must sit next to lvgl/)
  lvgl/                         LVGL 8.4.0          (src only)
  GFX_Library_for_Arduino/      Arduino_GFX 1.5.5   (src only)
  TCA9554/                      TCA9554 0.1.2
tools/arduino-cli.exe           not in git; build.ps1 falls back to arduino-cli on PATH
build.ps1                       build / upload script
```

## Build and upload
One-time on a new PC (installs the ESP32 core, `esp32:esp32@3.2.0`):
```powershell
.\build.ps1 -Setup
```
Then:
```powershell
.\build.ps1                          # compile
.\build.ps1 -Upload                  # compile + flash to COM8
.\build.ps1 -Upload -Port COM5       # different port
.\build.ps1 -Upload -Monitor         # flash, then open serial monitor
.\build.ps1 -Clean                   # full rebuild
```
If PowerShell blocks the script: `powershell -ExecutionPolicy Bypass -File .\build.ps1 -Upload`

If the board doesn't show up on any COM port: hold **BOOT**, tap **RESET**, release **BOOT**, then upload.

## Board settings
Pinned to the core version above. LVGL 8 and GFX 1.5.5 do not build on newer cores
(the core's SPI API changed in 3.3.x).

| Setting | Value |
|---|---|
| Board | ESP32S3 Dev Module |
| Flash Size | 16MB |
| Partition Scheme | 16M Flash (2MB APP/12.5MB FATFS) |
| PSRAM | OPI PSRAM |
| USB CDC On Boot | Enabled |

FQBN: `esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=fatflash,PSRAM=opi,CDCOnBoot=cdc`

## Using the Arduino IDE instead
Set **File > Preferences > Sketchbook location** to this folder, restart the IDE, and open
`tuna_ui/tuna_ui.ino`. Set the Tools menu as in the table above. Switch the sketchbook back
when you work on other projects: `Documents\Arduino\libraries` has LVGL 9, which does not
build this code.

## Serial protocol (ESP32 <-> Pi, 115200, newline-terminated)
| ESP32 sends | Pi replies |
|---|---|
| `CMD\|LIST` | `FILES\|a.gcode;b.gcode;...` |
| `CMD\|HOME` | `OK\|HOME` or `ERR\|<msg>` |
| `CMD\|RESTART` | `OK\|RESTART` or `ERR\|<msg>` |
| `CMD\|ESTOP` | (none) |
| `CMD\|PRINT\|<file>` | `OK\|PRINT` or `ERR\|<msg>` |

The Pi link and debug output currently share the USB `Serial`, so every command is followed by
a `>> CMD|...` echo line. The Pi side must ignore lines starting with `>>`, or `RPI_SERIAL`
must be moved to a hardware UART.

## Known issues
- **Touch driver.** `esp_lcd_touch_axs15231b.*` was written for this project; Waveshare's
  vendor file was not available. If taps land in the wrong place, check the rotation mapping
  in `bsp_touch_read()` first.
- **Orientation.** If the image is upside-down, change `LCD_ROTATION` in `tuna_ui.ino`
  (1 or 3). Touch follows it automatically.
