# Light Alarm — Project Documentation

## Overview

A sunrise alarm clock built on an ESP32. At a scheduled time, it connects to a BLE LED strip over Bluetooth and gradually ramps the light from black through deep red and orange to a warm sunrise color over a configurable duration (default 1 hour). A mobile-friendly web UI is served directly from the ESP32 — no external hosting, no cloud, no app.

---

## Hardware

| Component | Details |
|---|---|
| Microcontroller | ESP32 (generic dev board) |
| LED Strip Controller | `LEDDMX-00-6627` — BLE-controlled RGB strip |
| Connection | BLE (Bluetooth Low Energy) via NimBLE |
| Network | Home WiFi (2.4GHz) |

The ESP32 acts as a BLE **central** (client). It scans for the strip by device name, connects, and writes raw byte commands to a specific GATT characteristic.

### BLE Protocol

The strip uses a proprietary GATT service on two UUIDs:

- **Service:** `0000ffe0-0000-1000-8000-00805f9b34fb`
- **Characteristic:** `0000ffe1-0000-1000-8000-00805f9b34fb`

All commands are 9-byte arrays written without response (`writeValue(..., false)`):

| Command | Bytes |
|---|---|
| Power On | `7B FF 04 03 FF FF FF FF BF` |
| Power Off | `7B FF 04 02 FF FF FF FF BF` |
| Set Color (R, G, B) | `7B FF 07 R G B 00 FF BF` |

---

## Firmware Architecture (`src/main.cpp`)

Written in C++ using the Arduino framework for ESP32 via PlatformIO.

### Boot Sequence (`setup()`)

1. Connect to WiFi — blocks until connected, prints IP to Serial
2. Init NTP — syncs time from `pool.ntp.org` using the configured UTC offset (default EST, UTC-5 = -18000 seconds)
3. Mount LittleFS — serves the frontend UI from flash storage
4. Register HTTP API endpoints
5. Start the async web server on port 80
6. Init NimBLE device (scanning begins in `loop()`)

### Main Loop (`loop()`)

Runs continuously. Handles four independent concerns in sequence:

#### 1. BLE Connection
If `pChar` is null (not connected), scans for 5 seconds. If `LEDDMX-00-6627` is found, connects and resolves the characteristic. Retries every 2 seconds until connected. Once connected, this block never runs again (no reconnect logic — power cycle ESP32 if strip restarts).

#### 2. Fade Startup (`fadePending`)
When a ramp is triggered, `powerOn()` is called but `isFading` is NOT set immediately. Instead, `fadePending = true`. After 200ms, the loop snaps the strip to black (`setColor(0,0,0)`) and then sets `isFading = true`. This prevents a white flash — the strip remembers its last color on power-on, so without this delay the ramp would start from white, briefly dim, then ramp up.

#### 3. Reliable Repeat
`powerOn`, `powerOff`, and manual `setColor` calls use `sendReliable()` which sends the command immediately and queues 2 retries at 120ms intervals. This compensates for occasional BLE packet loss without flooding the strip. The sunrise ramp uses single `sendCommand()` calls since it sends updates at ~50ms intervals anyway.

#### 4. Pending Color Send
For the manual `/on` endpoint, `powerOn()` is called first, then a 150ms timer is set. After 150ms, `setColorReliable()` sends the target color. This gap is necessary because back-to-back BLE writes (power + color) can get dropped.

#### 5. Alarm Check
Every loop iteration (when not fading), checks current time against the set alarm. Uses `tm_mday` to reset the daily trigger flag when the calendar day changes — robust to missed midnight checks during BLE scan blocking. Triggers `startRamp()` when `hour:min` matches.

#### 6. Sunrise Ramp
When `isFading` is true, computes color every iteration using a **cubic ease-in curve**:

```
progress = elapsed / duration          // 0.0 → 1.0
curve    = progress³                   // cubic ease-in
R = RAMP_R * curve
G = RAMP_G * curve
B = RAMP_B * curve
```

Only sends a BLE command when the color value actually changes (avoids flooding). A `delay(50)` at the end of each ramp tick gives the BLE stack breathing room.

### Sunrise Color

Hardcoded as compile-time constants — **never modified by the UI**:

```cpp
const uint8_t RAMP_R = 255, RAMP_G = 60, RAMP_B = 10;
```

This produces a ramp from black → deep red → warm orange-red that mimics natural sunrise. The cubic curve means most of the brightness increase happens in the final third of the duration, matching real sunrise behavior.

---

## HTTP API

All endpoints are GET requests. The server adds `Access-Control-Allow-Origin: *` globally.

### `GET /start?time=<ms>`

Triggers the sunrise ramp immediately (manual use).

| Param | Type | Default | Description |
|---|---|---|---|
| `time` | integer (ms) | current `fadeDuration` | Duration of the ramp in milliseconds |

### `GET /stop`

Powers off the strip immediately. Clears all pending state (fading, fadePending, pendingColorSend).

### `GET /on?r=<0-255>&g=<0-255>&b=<0-255>`

Turns the strip on instantly at full brightness with the given color. Does **not** affect the ramp color. Uses the 150ms pending mechanism to ensure powerOn settles before sending setColor.

### `GET /setalarm?hour=&min=&duration=&enabled=&utcoffset=`

Configures the alarm. All parameters optional — only provided params are updated.

| Param | Type | Description |
|---|---|---|
| `hour` | 0–23 | Alarm trigger hour |
| `min` | 0–59 | Alarm trigger minute |
| `duration` | integer (ms) | Ramp duration |
| `enabled` | 0 or 1 | Arms/disarms the alarm |
| `utcoffset` | integer (seconds) | UTC offset, triggers NTP reconfiguration |

### `GET /status`

Returns current device state as JSON. Polled by the frontend every 5 seconds.

```json
{
  "time": "07:32",
  "alarmEnabled": true,
  "alarmHour": 7,
  "alarmMin": 0,
  "fadeDuration": 3600000,
  "isFading": false,
  "progress": 0.000,
  "utcOffset": -18000
}
```

---

## Frontend Architecture (`data/`)

Served from the ESP32's LittleFS flash partition. Three files: `index.html`, `style.css`, `app.js`. Vanilla HTML/CSS/JS — no framework, no build step.

Since the frontend is served from the same device the API runs on, all fetch calls use **relative URLs** (`/start`, `/stop`, etc.) — same origin, no CORS issues, no mixed content problems.

### UI Structure

**Header** — Large time display (from `/status`) + status pill showing current state.

**Alarm Card**
- Toggle switch to arm/disarm
- Wake time picker (native `<input type="time">` — opens system time picker on mobile)
- End time picker — duration is derived as `endTime - startTime`
- "Set Alarm" button — calls `/setalarm` with all current values

**Color Card** — For manual use only. Six presets: White, Warm, Amber, Red, Blue, Custom. Tapping a preset immediately calls `/on` with that color. Custom exposes a `<input type="color">` picker. **These colors never affect the sunrise ramp.**

**Progress Card** — Hidden when idle. Appears during an active ramp showing a live progress bar and percentage, updated every poll cycle.

**Manual Controls** — On (instant, uses selected color), Ramp (starts sunrise sequence), Off.

**Settings Card** — UTC offset input. Calls `/setalarm?utcoffset=<seconds>` on save.

### State Management

No framework. State lives in:
- `selR`, `selG`, `selB` — currently selected manual color (JS variables)
- Server state — polled every 5 seconds via `/status`, reflected to UI

### Polling

`setInterval(pollStatus, 5000)` — every 5 seconds the frontend fetches `/status` and updates the clock, status pill, and progress card. Fields that are currently focused are not overwritten (prevents cursor jumping in time inputs).

---

## Libraries

| Library | Version | Purpose |
|---|---|---|
| `h2zero/NimBLE-Arduino` | ^1.4.1 | BLE central (client) — lighter than built-in BLE library |
| `ESPAsyncWebServer` (me-no-dev) | latest | Non-blocking async HTTP server |
| `AsyncTCP` (me-no-dev) | latest | TCP backend for ESPAsyncWebServer |
| `LittleFS` | built-in | Flash filesystem for serving frontend files |
| `WiFi` | built-in | WiFi connection |
| `time.h` | built-in | NTP sync via `configTime()` / `getLocalTime()` |

NimBLE is used instead of the built-in ESP32 BLE library because it uses significantly less RAM and flash, leaving more headroom for the web server and LittleFS.

---

## PlatformIO Configuration

```ini
[env:esp32dev]
platform = espressif32
board = esp32dev
framework = arduino
board_build.filesystem = littlefs
lib_deps =
    h2zero/NimBLE-Arduino@^1.4.1
    https://github.com/me-no-dev/ESPAsyncWebServer.git
    https://github.com/me-no-dev/AsyncTCP.git
monitor_speed = 115200
```

`board_build.filesystem = littlefs` tells PlatformIO to use the LittleFS partition tool when building/uploading the filesystem image from the `data/` directory.

---

## Deployment

Two separate upload steps are required:

### 1. Firmware
PlatformIO → `esp32dev` → **Upload**
Compiles and flashes `src/main.cpp` and all dependencies.

### 2. Filesystem
PlatformIO → `esp32dev` → **Platform** → **Upload Filesystem Image**
Packages the `data/` directory into a LittleFS binary and flashes it to the filesystem partition.

The Serial Monitor (115200 baud) must be **closed** before either upload — it locks the COM port.

After boot, the assigned IP is printed to Serial:
```
IP: 192.168.1.157
```

Open `http://<IP>` on any device on the same WiFi network.

### When to re-upload

| Changed | Firmware | Filesystem |
|---|---|---|
| `src/main.cpp` | Yes | No |
| `data/*.html/css/js` | No | Yes |
| Both | Yes | Yes |

---

## Timezone

Default: **EST (UTC-5, -18000 seconds)**. Hardcoded in firmware as `utcOffsetSeconds = -18000`.

DST change: March — update the Settings card in the UI to `-4` (EDT) and tap Save. This reconfigures NTP without reflashing.

---

## Known Behaviors

- **BLE reconnect** — If the strip loses power and restarts, the ESP32 will not reconnect automatically. Power cycle the ESP32 to re-scan and reconnect.
- **Alarm state is not persisted** — All alarm settings (time, enabled state, duration) are lost on ESP32 reboot. The alarm must be re-set after any power cycle.
- **BLE scan blocks loop** — During the initial 5-second BLE scan, the loop is blocked. HTTP requests queue normally and are handled after the scan. Once BLE is connected, scanning never runs again.
- **NTP sync delay** — On fresh boot, NTP may take 5–15 seconds to sync. The status clock shows `--:--` until sync completes. The alarm will not fire during this window.
