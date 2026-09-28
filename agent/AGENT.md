# AGENT.md — changing the DGX rack fan controller

This is the guide for an AI agent (or a person) changing this project: the
firmware, the helper scripts or the GX-RACK server. It is self-contained —
the hardware facts and design decisions are below. See also
`../docs/how-it-works.md` for the behaviour in plain language and
`../controller/README.md` for wiring and pins.

**No credentials are written anywhere in the repository, and none may be.**
They live only in `controller/.env` (template: `controller/.env.example`,
keys `WIFI_SSID`, `WIFI_PASS`, `USER`, `WEBPASS`) and, for GX-RACK, in
`gx-rack/.env` on the Docker host. The scripts read `controller/.env`
themselves. Never print, echo, `cat`, commit or paste its values — to inspect
it, show key names only (`sed 's/=.*/=<hidden>/' controller/.env`). Wi-Fi and
web passwords are typed into the USB console by the user, never passed
through chat.

## What this is

A LOLIN S3 Mini (ESP32-S3) drives two Noctua 120 mm 4-pin PWM fans that blow
up into the bottom intakes of two ASUS Ascent GX10s. Each fan follows its own
10 kΩ NTC probe, taped at the back of that fan's shroud, in air the GX10 has
already heated. Fan 1 ↔ probe 1, fan 2 ↔ probe 2. Once installed, the usual
access is **Wi-Fi** (web page, JSON API, OTA updates); USB needs the board
brought to a computer.

## Where things are

| Path | What |
|------|------|
| `controller/src/main.cpp` | the whole firmware, one file, in sections (below) |
| `controller/platformio.ini` | build envs: `lolin_s3_mini` (USB, default), `lolin_s3_mini_ota` (Wi-Fi), `lolin_s3` (full-size board) |
| `controller/README.md` | wiring, pins, console commands, calibration |
| `docs/` | how it works, components, wiring drawings + `render.py` |
| `agent/fans.sh` | read status / change profile / identify / turn fans off |
| `agent/ota-upload.sh` | build and install the firmware over Wi-Fi |
| `gx-rack/` | GX-RACK: optional dashboard + REST + MCP server (Docker) |
| `agent/deploy-gx-rack.sh` | copy `gx-rack/` to a Docker host over ssh and rebuild it |

## Hardware facts — verified, don't re-open them

- **Board:** LOLIN S3 Mini v1.0.0 (`ESP32-S3FH4R2`, 4 MB flash, 2 MB PSRAM).
  The full-size LOLIN S3 (`ESP32-S3-WROOM-1 MCN16R8`, octal PSRAM, so GPIO
  26–37 are unusable) is also supported with the same pins.
- **Pins:** `1`/`2` thermistors (ADC1, keeps working with Wi-Fi on),
  `11`/`12` fan PWM, `13`/`14` fan tach, `21` on/off switch, `0` profile
  button (the board's own).
- **Probes:** plain 10 kΩ NTC thermistors @ 25 °C, 2-wire, read through a
  divider. Beta isn't published; the firmware assumes 3950.
- **Fans:** Noctua 4-pin, 2 tach pulses per turn, 25 kHz PWM, stall below
  about 20 % duty. 3.3 V PWM drives them directly.
- Noctuas run at **100 % when the PWM wire is undriven**. A 1 kΩ pull-down
  from each fan's blue wire (fan side of the 100 Ω) to GND makes them stop
  when the controller is off, without affecting speed control.
- The ESP32-S3 ADC stops tracking above about **3100 mV** at 12 dB
  attenuation. The probe-fault window must stay well inside that (currently
  200–2800 mV), or an unplugged probe reads as "very cold" instead of FAULT.
- **Tach** is read by the PCNT hardware counter with the glitch filter maxed.
  Plain `attachInterrupt` edge counting picks up PWM crosstalk and over-reads
  RPM by up to ~50 %. Don't "simplify" it back.
- The S3 Mini's RGB LED has red and green swapped; corrected under
  `ARDUINO_LOLIN_S3_MINI`.
- `platform = espressif32` pins Arduino core 2.0.17, so the
  `ESP_ARDUINO_VERSION_MAJOR >= 3` branch of the LEDC shim is dead code, kept
  for a future platform bump.
- First USB flash of an S3 Mini running other firmware: hold `0`, tap `RST`,
  release `0`.

## Design decisions — deliberate, keep them unless asked

- **No full-speed failsafe:** a faulted probe stops its own fan; fans boot
  stopped (no 100 % burst on power-up).
- **Curve** is for exhaust air: off below 30 °C, 20 % at 30 °C → 100 % at
  55 °C. Overheat point 55 °C.
- **Profiles:** Normal = curve capped at 80 %; Quiet = curve × 67 % (so max
  67 %), even when hot; only Max reaches 100 %. Always boots Normal.
- **Controls:** the on/off switch is self-locking (in/lit = on). The `0`
  button: single press Normal ↔ Quiet, double Max, long → Normal. LED: green
  Normal, blue Quiet, red Max, amber switch off. The 55 °C overheat overrides
  only the switch-off.
- **Turn off** (per fan, web/API/console) is a hard stop that outranks
  everything, heat included. Not saved across reboot; flipping the physical
  switch clears it.
- **Console `set`/`max`** outranks the probe-fault stop so fans can be
  bench-tested; the hold expires after 10 minutes.
- **Network code never blocks** the fan control loop.

## Sections of `main.cpp`

Top to bottom (search for the `// ---- name --` line):

| Section | Change it for |
|---------|---------------|
| `pin map` | pins (then redraw `docs/wiring/`) |
| `thermistors` | `NTC_R25`, `NTC_BETA`, divider, fault window (200–2800 mV), `TEMP_EMA` smoothing |
| `fans` | `DUTY_MIN` 20, `DUTY_START` 50 kick, `DUTY_SLEW` 1 %/tick, PWM freq, tach filter |
| `response` | **the temperature curve** `CURVE[]`, `FAN_OFF_BELOW_C`, `HYSTERESIS_C` |
| `profiles` | `QUIET_PCT` 67, `NORMAL_MAX_PCT` 80, `OVERHEAT_C` 55, button timings, `ZONE_MODE`, `MANUAL_TIMEOUT_MS`, `IDENTIFY_MS` |
| `state` | globals, incl. `identifyFan`, `fanOff[]` |
| `tach` / `pwm` | hardware counter and LEDC shim — leave alone (see above) |
| `profile + led` | `setProfile()`, LED colours |
| `on/off sw` / `button` | switch debounce, button gestures |
| `network` | the web page (`PAGE[]`, inline HTML/JS), `/api/*` handlers, OTA |
| `serial` | USB console commands |
| `loop` | **the per-fan decision** — priority order below |

### Per-fan priority in `loop()` (highest first)

1. **Turned off** (`fanOff[i]`) → 0 % at once. Nothing overrides it, not even heat.
2. **Identify** (`identifyFan`) → that fan 100 %, the other 0 %, no slew, 15 s.
3. **Console hold** (`set`/`max`, `manualMode`) → fixed duty, expires after 10 min.
4. **Max profile** with the switch on → 100 %.
5. **Probe fault** → 0 % (no full-speed failsafe).
6. **Curve**: switch off → 0 % unless the probe is ≥ 55 °C; Quiet → curve × 67 %; Normal → curve capped at 80 %.

Levels 3–6 then pass through the slew limit (1 % per 100 ms), the 50 % restart
kick and the stop hysteresis. Levels 1–2 bypass it.

## Common changes

- **Curve**: edit `CURVE[]` (ascending, `{°C, %}`) and `FAN_OFF_BELOW_C`
  (the first point should sit at it). Duties below 20 are not allowed except 0.
  Update the table in `controller/README.md` and `docs/how-it-works.md`.
- **Profile caps**: `QUIET_PCT`, `NORMAL_MAX_PCT`. The page text is static;
  grep `PAGE[]` for numbers you change (e.g. `55 °C`).
- **New web control**: add a handler next to `handleFanPost()`, start it with
  `if (!webAuthorised()) return;`, register it in `netStartServices()`, add any
  new state to `handleStatus()` (grow `json[]` if needed), then a button + a
  `fetch()` in `PAGE[]`. Keep every handler non-blocking.
- **New console command**: `handleCommand()` and its `help` line.

## Rules

- The hardware facts and design decisions above are settled; change a
  decision only when the user asks for it, and then update this file.
- Match the code's style: short comments that say *why*, constants at the top
  of their section.
- These fans cool real machines under load. Before a test that stops a fan
  or changes the profile, say so; afterwards put back what the user had
  (check `agent/fans.sh status` first). Someone may be pressing buttons on
  the page at the same time — if state changes under you, stop and ask.
- Keep docs in step with behaviour: `controller/README.md`,
  `docs/how-it-works.md`, this file, and GX-RACK (below).
- Wiring drawings: edit the `.excalidraw` (in Excalidraw, or its JSON text
  elements), then `python3 docs/wiring/render.py` to regenerate the PNGs
  (needs Chrome or Chromium). Look at the PNG before calling it done.

## Build, install, verify

PlatformIO is expected at `~/.platformio/penv/bin/pio` (the scripts use that
path; it is often not on `PATH`).

```
agent/ota-upload.sh --build     # compile only — do this after every edit
agent/ota-upload.sh             # compile, install over Wi-Fi, wait for it
agent/fans.sh status            # uptime should be a few seconds after install
```

Over USB instead: `cd controller && ~/.platformio/penv/bin/pio run -t upload`,
console with `pio device monitor` (115200).

The controller is found as `dgx-fans.local` (mDNS); set `DGX_HOST=<ip>` if
that doesn't resolve. OTA needs the controller to connect *back* to the PC.
If your router blocks that (e.g. the controller on a separate IoT or 2.4 GHz
network), espota authenticates and then says "No response from device".
`ota-upload.sh` handles this on Linux with NetworkManager: if the PC's Wi-Fi
is on another subnet, it switches to a saved profile for the controller's
network (`WIFI_SSID`) for the upload and switches back afterwards.

An install restarts the controller: per-fan "turned off" is cleared and the
profile returns to Normal. If the user had something else set, put it back.

Other checks:

```
agent/fans.sh watch 20          # watch temps, duty and RPM settle
agent/fans.sh identify 1        # fan 1 at 100 %, fan 2 stopped (15 s)
agent/fans.sh identify stop
agent/fans.sh profile quiet     # normal | quiet | max
agent/fans.sh fan 2 off         # on | off
```

Expected RPM: ~660 at 20 %, ~1400 at 45 %, ~1900–2000 at 60–67 %, ~2400 at
80 %, ~3000 at 100 %. A running fan at 0 RPM means a tach or fan problem.

## GX-RACK (dashboard + MCP server)

`gx-rack/` is a small Python container (Starlette + the `mcp` SDK) that polls
the controller and serves a dashboard, REST and the MCP server "GX-RACK".
Setup, exposure and agent config: `gx-rack/README.md`.

- Its `.env` (controller login, MCP key) exists **only on the Docker host**
  and is never copied back or printed. `deploy-gx-rack.sh` never touches it.
- **When firmware behaviour changes** (curve, caps, controls, API fields),
  update `SYSTEM` and `summary()` in `gx-rack/app/main.py` and the dashboard
  text in `gx-rack/app/static/index.html`, then redeploy.
- A new firmware control needs three things there: a helper next to
  `fan_power()`, an `@mcp.tool()`, and a REST route that checks `authorised()`.
- Test locally first: `docker build -t gx-rack:local gx-rack` and run it
  with a scratch env file.

## If Wi-Fi is gone

The controller keeps controlling the fans without the network. It retries
Wi-Fi every 30 s. If it never comes back, it has to go on USB; the user types
the Wi-Fi and web passwords into the console themselves (`wifi ssid …`,
`wifi pass …`, `webpass …`).
