# Project Prompt

## Non-negotiable requirements
Use native ESP-IDF (not Arduino) and keep the project directly usable with the Espressif VS Code extension.

## Source of Truth

The existing source code is the only authoritative specification for this project.

When documentation, comments, generated files, assumptions, or external examples disagree with the source code, follow the source code. Do not silently change working behavior to match documentation. Update documentation only when the source implementation has intentionally changed.

Preserve existing functionality and architecture unless a requested change requires otherwise. Prefer the smallest focused change that fits the existing implementation.

## Project Identity

This is a native ESP-IDF C application for an ESP32 target.

The hardware target is:

- M5Stack Core Basic v2.7
- ESP32
- 320x240 ILI9342C display
- M5Stack GPS Module v2.0 using an AT6668 GNSS receiver

Do not introduce Arduino, M5Unified, TinyGPS++, or other external frameworks or components unless explicitly requested.

## Language And Formatting

- Use C for application and component code.
- Use Allman brace style.
- Use two spaces for indentation.
- Use lowerCamelCase for functions and variables where the surrounding public API permits it.
- Use descriptive names; do not use one-letter variable names except for tightly scoped conventional indices.
- Preserve existing whitespace and formatting in unrelated code.
- Do not perform broad reformatting.
- Use standard C types and ESP-IDF types consistently with the existing code.
- Prefer `std::string` only in C++ code; this project is C and should use C strings and fixed buffers as already implemented.

## Comment Rules

Comments must never appear beside code on the same line.

A comment must always appear above the code line, code block, or function it describes.

Never use block comments. Do not use `/* ... */` comments anywhere in new or modified code.

Every comment must begin with the exact prefix `//-- `.

Example:

```c
//-- Configure the display pins before initializing the SPI bus.
configure_display_pins();
```

Keep comments concise and useful. Do not add comments that merely restate obvious code. Existing comments are part of the current source reference, but any new or modified comments must follow these rules.

## ESP-IDF Rules

- Use ESP-IDF APIs, component registration, FreeRTOS, and ESP-IDF types.
- Keep ESP-IDF component dependencies explicit in each component's `CMakeLists.txt`.
- Use the split ESP-IDF driver components where required by the installed ESP-IDF version.
- The board component currently uses the legacy I2C API through the `driver` component and also requires the split GPIO and I2C components.
- The GPS component uses `esp_driver_uart`.
- The LCD component uses `esp_driver_gpio` and `esp_driver_spi`.
- Do not mix Arduino APIs into this project.
- Do not invent ESP-IDF APIs, configuration options, or component names.
- Validate changes with an ESP-IDF build.
- Never flash or upload firmware automatically. The user performs flashing manually.
- idf.py is located in `$HOME/.espressif/tools/activate_idf_v6.0.2.sh`

## Hardware Pinout

### LCD

The native LCD driver is in `components/lcd/lcd.c` and uses SPI3:

- MOSI: GPIO23
- SCLK: GPIO18
- CS: GPIO14
- DC: GPIO27
- RESET: GPIO33
- Backlight: GPIO32
- SPI mode: 0
- SPI clock: 40 MHz
- Display size: 320x240
- Pixel format: RGB565

The current display orientation is controlled by the ILI9342C MADCTL command. The working value in the source is `0x08`. Preserve this value unless the physical display orientation is deliberately revalidated on the actual M5Stack hardware.

The display has the buttons physically below it. Preserve the current top-to-bottom screen geometry and readable text orientation.

The LCD is driven directly through SPI. Do not replace it with a graphics framework or a different display abstraction without an explicit request.

The LCD and SD card share the M5Stack VSPI bus on SPI3. The shared bus pins are MOSI GPIO23, MISO GPIO19, and SCLK GPIO18. LCD CS is GPIO14 and SD CS is GPIO4. Do not initialize a second SPI bus for the SD card or change either device's chip-select pin.

### LCD Color Correctness (confirmed on hardware)

This specific ILI9342C panel requires Display Inversion mode to be explicitly turned **ON** during init, or every color renders as its bitwise-inverted opposite (for example intended white renders as black, intended green renders as purple). This was diagnosed and confirmed using `lcd_color_test()` in `components/lcd/lcd.c`, which draws labeled RGB565 bars on screen for a physical photo comparison.

Required and confirmed-working fix:

- `lcd_init()` in `components/lcd/lcd.c` must send `cmd(0x21)` (Display Inversion ON) during panel initialization, after the gamma/COLMOD setup and before sleep-out (`cmd(0x11)`). Do not remove this command or change it to `0x20`.
- With `cmd(0x21)` sent, `LCD_COLOR_*` macros in `components/lcd/include/lcd.h` use **true, standard RGB565 values** (no byte-swapping or bit-inversion pre-correction needed).

Confirmed primary/secondary color macros (standard RGB565, verified correct on the physical display with `cmd(0x21)` active):

```c
#define LCD_COLOR_BLACK    0x0000
#define LCD_COLOR_WHITE    0xFFFF
#define LCD_COLOR_RED      0xF800
#define LCD_COLOR_GREEN    0x07E0
#define LCD_COLOR_BLUE     0x001F
#define LCD_COLOR_YELLOW   0xFFE0
#define LCD_COLOR_CYAN     0x07FF
#define LCD_COLOR_MAGENTA  0xF81F
```

Rules for future color changes:

- Always add new UI colors as a named `LCD_COLOR_*` macro in `lcd.h` using a true, standard RGB565 value. Never use a raw hex color directly in draw calls.
- Never remove or bypass `cmd(0x21)` in `lcd_init()`. If colors ever look wrong again on this panel, verify Display Inversion state first before assuming a color macro or byte-order problem.
- If a different physical display panel is ever substituted, re-run the `lcd_color_test()` bar test and re-confirm before trusting these values.

### Buttons And Battery

The board component uses:

- Button A: GPIO39
- Button B: GPIO38
- Button C: GPIO37
- I2C bus: I2C0
- I2C SDA: GPIO21
- I2C SCL: GPIO22
- IP5306 address: `0x75`

Buttons are active low. Button events are debounced and long presses are recognized by the board task. A long press must execute immediately once the long-press threshold is reached; it must not wait for the button to be released. Short presses remain valid after the debounce threshold and are only generated on release.

### GPS

The GPS configuration is defined in `main/app_main.c` and must remain consistent with the hardware:

- UART: UART2
- GPS RX into ESP32: GPIO16
- GPS TX from ESP32: GPIO17
- Baud rate: 115200
- Frame: 8N1
- Hardware flow control: disabled
- Position update request: 10 Hz through `$PCAS02,100*1E`

The GPS parser accepts checksummed NMEA RMC and GGA sentences. Preserve checksum validation, fix handling, satellite count handling, and the thread-safe latest-data interface.

## SD Card GPS Export

Valid GPS fixes are processed for the active trip. The first valid fix is written.
After that, a new fix is written every second unless it is skipped:

- Skip when the fix reports a speed below `STATIONARY_SPEED_KMH` (0.5 km/h) and the
  previously written fix was also below that speed.
- Skip when the coordinate distance to the previously written fix is below
  `MIN_MOVEMENT_METERS` (3.0 meters).

These thresholds are defined in `components/sdcard/sdcard.c`. This coordinate-distance
check is independent of the integrated trip distance used by the TRIP display.

Each trip must use a new GPX file and CSV file when the trip is reset. Both files use this exact base name:

```text
trip-EEYYMMDD-HHmmSS.gpx
trip-EEYYMMDD-HHmmSS.csv
```

- The numeric identifier `EEYY` is the Centery+Year (2025, 2026 etc.).
- The numeric identifier `MM` is the Month (01 .. 12)
- The numeric identifier `DD` is the current Day (01 .. 31).
- The numeric identifier `HH` is the current Hour (01 .. 24).
- The numeric identifier `mm` is the current Minute (00 ..59).
- The numeric identifier `SS` is the current Second (00 ..59).

The date/time must be collected from the GPS and converted to the local civil time implied by the current GPS position. Determine the active time zone from the GPS coordinates and local daylight-saving rules, then convert the GPS UTC/Zulu time to that local time. For the Netherlands this resolves to `Europe/Amsterdam`, but the implementation should derive the zone from position rather than hard-coding a single timezone.

If the SDcard has less then 20% free space remove the oldest files until there is again more then 20% free space.

GPX must contain valid track points, and CSV must contain the date, time, latitude, longitude, altitude, speed, course, satellite count, and cumulative distance for each exported position. Write only selected valid GPS fixes and handle file-open, write, sync, close, and card errors explicitly.

The active GPX path is stored in NVS. On startup, the storage component resumes that exact GPX/CSV pair instead of truncating a file with the current minute's name. Existing GPX closing tags are removed before appending new positions. Trip reset must close both current export files before creating the next pair. The new files must be initialized with valid GPX/CSV structure before positions are appended, and the GPX closing tags must be written after every exported position and when the trip ends or the application shuts down where the platform allows it.

The GPX file descriptor must be opened `O_RDWR`, not `O_WRONLY`. Removing the closing tags before every append reads the current tail of the file on that same descriptor to locate `</trkseg>`; a write-only descriptor fails that read with `EBADF`, silently preventing every position from being recorded. The CSV descriptor stays `O_WRONLY` since it is never read back.

If the closed trip-file has less then 20 entries, delete it before opening an new trip-file. This deletion is currently disabled by the temporary `SDCARD_KEEP_SMALL_TRIP_FILES` debug switch at the top of `components/sdcard/sdcard.c`, kept at `1` while the GPX/CSV export bug is being diagnosed on hardware. Set it back to `0` to restore the 20-entry cleanup once the fix is confirmed.

The implementation must use the project's actual SD-card hardware and ESP-IDF support. Do not invent SD-card pins, mount points, host settings, or APIs. Add the required component dependencies explicitly and keep SD-card ownership in a dedicated component unless the existing architecture provides a better local owner.

## Component Responsibilities

### `main`

`main/app_main.c` owns application orchestration:

- Initialize NVS, board, LCD, GPS, and speedometer services.
- Handle button events.
- Select current speed versus trip average in SPEED mode and in the lower part of TRIP mode.
- Select TRIP mode versus SPEED mode for the main display.
- Provide the current trip distance in the lower part of SPEED mode.
- Manage display backlight timeout.
- Initialize TOTAL distance at 0 on startup; the current implementation does not persist TOTAL distance through NVS.
- Refresh the LCD at the existing cadence.
- Pass the integrated trip distance to SD-card export and display the processed-position count.

Do not move component responsibilities into `app_main.c` unless necessary.

### `components/board`

Owns button input, button event generation, I2C initialization, and IP5306 battery and charging status.

### `components/gps`

Owns UART configuration, GNSS command transmission, NMEA parsing, checksum validation, and synchronized latest GPS data.

### `components/lcd`

Owns the native ILI9342C SPI protocol, display initialization, primitives, custom glyphs, seven-segment speed digits, screen layout, colors, and rendering state.

The display uses a custom small bitmap glyph renderer. Do not replace it with a font library unless explicitly requested.

Current UI dimensions and layout are source-defined and must be treated as authoritative. The large speed readout uses custom seven-segment digits. Smaller labels use the existing bitmap glyph renderer and current scale values.

The second information bar shows the current TRIP distance or speed view and
the processed export-position count as an unlabeled, right-aligned number.

### `components/speedometer`

Owns GNSS speed filtering, display speed limiting, trip distance, total distance, trip reset, and trip average calculations.

Distance is integrated from valid GNSS speed. Preserve the current stationary threshold, filtering behavior, timing safeguards, and range limits.

### SD-card storage

The SD-card storage implementation owns card mounting, free-space reporting, trip file naming, GPX/CSV file lifecycle, coordinate appends, flushing, and error handling. GPS parsing and display rendering must not directly own SD-card protocol details.

Free-space reporting must use the mounted FatFs volume and `f_getfree()`. Do not use `statvfs()` for SD-card capacity reporting because the selected ESP-IDF version does not implement it and returns `ENOSYS`. A mounted card must remain usable even when a filesystem-statistics read fails; report the error without crashing.

Formatting must use the ESP-IDF SD-card FAT formatter. The current trip files must be closed before formatting, and a new GPX/CSV pair must be created after formatting succeeds.

## User Interface Behavior

The current controls are source-defined:

- Short Button A activates TRIP mode.
- Long Button A resets the active trip and creates the next `trip-EEYYMMDD-HHmmSS.gpx` and `.csv` export files using the current GPS date-time stamp.
- Short Button B toggles the display backlight on or off when the system menu is closed.
- Long Button B opens or closes the system menu.
- Short Button C activates SPEED mode and toggles between SPEED and AVG SPEED.
- When the backlight is off, a press on any Button A, B, or C wakes the backlight and is consumed without performing that screen's button action. The next press performs the normal action for the active screen.
- In the main screen, a short Button B press turns the backlight off; a subsequent short Button B press wakes it.
- If the system menu, an action screen, [List Trips], or [Trip Info] has no button activity for more than 60 seconds, the application returns to the main screen. The [WiFi Menu] is excluded and remains active until a LONG-press on Button B.

The position count shown on the second information bar is the number of
positions successfully written to the active GPX/CSV export pair.

Every button press/release is logged with the button name including its physical position (`A (LEFT)`, `B (MIDDLE)`, or `C (RIGHT)`), `SHORT` or `LONG` press classification, and press duration. Menu cursor changes and selected actions are also logged.


### System Menu

Opening the [System Menu] must not start WiFi or the webserver. WiFi and the webserver are only started from the [WiFi Menu].

When the [WiFi Menu] is entered, that menu stays active until a LONG-press on Button B is detected. It does not close on a short-button action or on key release.

When the WiFi access point is not found and the device falls back to captive-portal mode, the WiFi screen must show the following text in a vertically spaced layout that starts high enough on the display to avoid overlapping the footer:

- `Captive portal: Active`
- `In settings select`
- `<hostname>`
- `Browse to 192.168.1.4`
- `to set WiFi Credentials`

The active hostname in this project is `tripTracker`. The footer text `Long MidKey: Close` must remain visible and must not overlap any of the captive-portal instructions. Use the existing display layout and font scale as the source of truth for spacing.

While the system menu is open, the normal application functions of all buttons are disabled:

- Short Button A moves the purple cursor up.
- Short Button C moves the purple cursor down.
- Short Button B executes the function under the cursor.
- Long Button B closes the system menu.

The system menu currently has 7 items but only 6 fit on screen at once. When the
cursor moves past the visible window, the menu scrolls so the cursor stays
visible; moving the cursor back scrolls the window back accordingly.

The menu options, in cursor order, are:

1. `New Trip File`: resets the active trip and creates new GPX and CSV files named with the current GPS date-time in the `trip-EEYYMMDD-HHmmSS` format.
2. `Show Used and Free`: shows the actual used and free SD-card space, auto-scaled to GB/MB/KB, on the `SD CARD INFO` Action screen.
3. `WiFi Menu`: enters `[WiFi Menu]`.
4. `Reset Tracker`: restarts the device (`esp_restart()`).
5. `Format SDcard`: formats the SD card, creates a new GPX/CSV trip-file pair, and returns to the system menu after formatting succeeds.
6. `List Trip Files`: opens `[List Trips]`, a scrollable list of all `.gpx` trip files showing date/time and total distance (right-aligned).
7. `Exit`: closes the system menu.

### System Menu And WiFi Menu Appearance

The `[System Menu]`, `[WiFi Menu]`, `[List Trips]`, and the `Show Used and Free` / `New Trip File` action screens share the same header layout, drawn by the shared `draw_header()` helper in `components/lcd/lcd.c`:

- The heading is drawn top-left at font scale 2 in cyan (`SYSTEM MENU` / `WiFi MENU` / `List Trips` / `SD CARD INFO` / `NEW TRIP FILE`).
- The firmware version string (`PROG_VERSION` from `main/app_main.c`, passed through `lcd_view_t.prog_version`) is drawn right-aligned at font scale 2 in white on the same header row. In `[System Menu]`, this is replaced by a `Wifi` (green) or `Ap-mode` (yellow) indicator only while WiFi is actually connected or running its AP fallback; the version shows otherwise. The remaining `Executing` action screens (`Wifi menu`, `Reset Tracker`, `Format SDcard`) also show the version top-right, without the rest of the shared header.
- A dark-grey 1px horizontal separator line is drawn directly below the heading, spanning the full screen width.
- Menu item text does not use letter-key prefixes; each item is drawn as plain title-cased text (`New Trip File`, `Show Used and Free`, `WiFi Menu`, `Reset Tracker`, `Format SDcard`, `List Trip Files`, `Exit`) at font scale 2.
- The item under the cursor is drawn in purple; unselected items are drawn in yellow.
- General on-screen UI text uses mixed/title case, not all-capitals, except for short fixed-width status labels on the main display (`GPS`, `NO GPS`, `SAT:nn`, `TRIP`, `SPEED`, `AVG SPEED`, `SD ERR`) which remain upper-case.
- The bitmap glyph renderer in `components/lcd/lcd.c` has distinct lowercase letter shapes (with true descenders for `g`, `j`, `p`, `q`, `y`) separate from the upper-case shapes, so mixed-case text renders differently from all-caps text.

Selecting `WiFi Menu` in `[System Menu]` transitions straight to `[WiFi Menu]`; it does not pass through the generic `menu_action`/`Executing` flow, so no interim action screen is shown.

Before `[WiFi Menu]` starts, all trip file pairs whose `.gpx` file is smaller
than 5120 bytes are deleted (both the `.gpx` and its matching `.csv`). The
trip currently being recorded is never deleted by this check, even if its
current `.gpx` file is still below that size.

After a short Button B execution on the remaining actions, the display is cleared and shows an Action screen. `Reset Tracker` and `Format SDcard` show `Executing` and the selected operation. `New Trip File` shows the `NEW TRIP FILE` header, the label `New File`, and the new file's `EEYYMMDD-HHmmSS` date/time (from `sdcard_get_active_trip_datetime()`) instead of `Executing`. `Show Used and Free` shows the `SD CARD INFO` header with `Used:` (red, left-aligned) above its indented white value, and `Free:` (green, left-aligned) above its indented white value, or `Sd unavailable` in red when the card is not mounted. Each value is auto-scaled by `format_storage_bytes()` to GB (2 decimals), MB, or KB, whichever fits best, with `,` thousand separators. Action screens remain visible until another short Button B press returns to the system menu, except that a successful `Format SDcard` action returns automatically to the system menu, and a `List Trip Files` action shows `Executing` briefly and then opens `[List Trips]` automatically.

Every button release is logged with the physical position, button name, `SHORT` or `LONG` press classification, and press duration. Menu cursor changes and selected actions are also logged.

#### In TRIP mode:

- Show the trip distance as the large central value using the same large seven-segment digit style as the speed value.
- Show `SPEED` or `AVG SPEED` at the left of the lower display area, followed by the current or average speed value.
- Keep the SD-card free-space indicator in the lower display area.

#### In SPEED mode:

- Show current speed or average speed as the large central value using large seven-segment digits.
- Show `TRIP` at the left of the lower display area, followed by the trip distance value.
- Keep the SD-card free-space indicator in the lower display area.

The main-mode labels `TRIP`, `SPEED`, and `AVG SPEED` are rendered horizontally. In the lower display area, the label must be placed to the left of its value. The `AVG SPEED` label must not overlap the speed value; the value must move to the right when the wider label is shown.

The display also shows:

- GPS fix state.
- Satellite count.
- Battery level.
- Charging state.

The lower display area must include a storage indicator bar showing the remaining usable SD-card space. Used space must be black and free space must be green. The bar must update from actual mounted-card FatFs capacity and free-space values, not from a hard-coded estimate. Handle an absent, unmounted, or unreadable card with a clear red error state without crashing the application. SD-card error text must be rendered in red consistently.

The main display layout uses a 320x240 screen. The separator below the middle section is rendered below the large seven-segment digits, and the large digits are positioned low enough that the `TRIP`, `SPEED`, and `AVG SPEED` headings remain readable without overlap.

The backlight timeout depends on battery level and is disabled while charging. Preserve the existing timeout behavior in `app_main.c`.

TOTAL distance is currently runtime-only and is initialized to 0 on startup; there is no NVS persistence for TOTAL distance in the current implementation.

### List Trip Files / `[List Trips]`

Selecting `List Trip Files` in the `[System Menu]` opens `[List Trips]`:

- The heading `List Trips` is drawn top-left at font scale 2 in cyan, with the
  same dark-grey 1px separator line style as `[System Menu]`/`[WiFi Menu]`.
- Only `.gpx` trip files are listed, sorted newest first.
- Each row shows the trip's date/time left-aligned and its total distance
  right-aligned, in the form `DD-MM-EEYY HH:mm   121 M` (meters, no
  decimal, below 1000 m) or `DD-MM-EEYY HH:mm   30.3 KM` (kilometers, one
  decimal, at or above 1000 m). The seconds are not shown in this list.
- If no trip files exist, a centered `No trip files` message is shown instead
  of a list.
- A cursor selects one row at a time. Short Button A moves the cursor up and
  short Button C moves the cursor down; the list scrolls to keep the cursor
  visible when there are more trip files than fit on screen. The selected
  row's date/time text is shown in white; unselected rows are shown in
  yellow.
- Short Button B opens `[Trip Info]` for the selected trip.
- Long Button B closes `[List Trips]` and returns to `[System Menu]`.

### Trip Info / `[Trip Info]`

Short Button B on a selected row in `[List Trips]` opens `[Trip Info]`, using
the same header layout as `[System Menu]` (title top-left in cyan, firmware
version top-right in white, dark-grey separator line beneath).

The screen shows, as yellow labels with white values:

- `Date`: the trip's start date (`DD-MM-EEYY`).
- `Time`: the trip's start and end time as `HH:mm - HH:mm` (no seconds),
  derived from the trip's filename date/time and the last recorded CSV
  timestamp.
- `Distance`: the trip's total distance, auto-scaled to meters or kilometers
  using the same rule as the `[List Trips]` row.
- `Duration`: total trip duration as `HH:MM:SS`, derived from the first and
  last recorded CSV timestamps.
- `Avg Speed`: total distance divided by total duration, in km/h.
- `Alt Diff`: the highest recorded altitude minus the lowest recorded
  altitude, in meters.

These values are computed by `sdcard_get_trip_details()` in
`components/sdcard/sdcard.c`, which reads the trip's CSV export file. If the
CSV file cannot be read, a red `Trip data unavailable` message is shown
instead.

Long Button B closes `[Trip Info]` and returns to `[List Trips]`. Short Button B
also closes `[Trip Info]` and returns to `[List Trips]`.

### WiFi Menu

The top part of the screen shows "WiFi MENU", styled the same as the `[System Menu]` heading (see above).

The [WiFi Menu] must always show a clear state/status message describing what it is doing. The text "EXECUTING WIFI MENU" is not valid and must not be used. The currently implemented status texts on the LCD are:

- `Connecting to ap` while attempting to join the known access point.
- `Captive portal` / `Active` when the captive portal fallback is running.
- `Connected to:` with the SSID and IP address, plus `Webserver:` / `Active` once the webserver is up.
- `Long MidKey: Close` as the persistent bottom-of-screen close hint.

WiFi and the webserver are only active while the [WiFi Menu] is open. In the normal application display or in the [System Menu], the webserver must be stopped and WiFi must be off to minimize power usage. The [WiFi Menu] must not close itself automatically because that would drop the WiFi connection. It remains active until a LONG-press on Button B is detected.

While the [WiFi Menu] is open, the normal application functions of all buttons are disabled except for:

- Long Button B closes the [WiFi Menu] and returns to [System Menu].

Entering the menu activates:

1 - Print "Connecting to AP", Start WiFi with known credentials. If after trying it is not possible to connect to the AP print "`Starting Captive Portal`", "`Select WiFi network <hostName> and browse to 192.168.1.4`", Start the Captive Portal.

2 - If connecting to the AP succeeds print "`Connected to <SSID> with <IP-ADDRESS>`"

3 - Start webserver (GUI) and print "`Browse to <hostName>.local`"

4 - If a client connect to the webserver (GUI) print "Accessed by `<IP-address client>`"

The [WiFi Menu] remains active until a LONG-press on Button B is detected; a short press does not close it. Long-press actions must be executed immediately once the threshold is reached, not after key release.

Trip filenames must use the GPS date/time when a valid GPS date is available, because that is the source of truth for the trip export. If a valid GPS date is not available yet, log a warning and continue using the best available fallback timestamp without aborting the trip file creation.

## Validation

After code changes:

1. Build the project with the ESP-IDF VS Code integration or an ESP-IDF terminal.
2. Confirm that the application and bootloader build successfully.
3. Confirm that the application binary fits the configured app partition.
4. Do not run flash, upload, erase, or monitor commands automatically.
5. Report any remaining compiler warnings or validation limitations clearly.

## Change Discipline

Make focused edits. Do not remove existing functionality. Do not change hardware pin assignments, display orientation, protocol settings, persistent-storage keys, or user controls without explicit evidence from the source and a direct task requirement.

## Miscalanious

The `idf.py` command is in `source "$HOME/.espressif/tools/activate_idf_v6.0.2.sh"`

