# DGX rack fan controller

LOLIN S3 Mini (ESP32-S3) driving two Noctua 120 mm 4-pin PWM fans from two
InLine 36219I thermistor probes, cooling two ASUS Ascent GX10s. (The
full-size LOLIN S3 is still supported: build env `lolin_s3`.)

## Bill of materials

The full parts list — S3 Mini, fans, probes, switch, every resistor with its
value, colour bands and purpose — is in
[`../docs/components.md`](../docs/components.md).

## Wiring

Noctua 4-pin, looking into the connector with the tab down — pin 1 is the
black wire:

| Pin | Wire   | Signal |
|-----|--------|--------|
| 1 | black  | GND |
| 2 | yellow | +12 V |
| 3 | green  | Tach (open collector) |
| 4 | blue   | PWM in |

### Power

```
12 V PSU (+) ─────────────► fan pin 2  (yellow, both fans)
12 V PSU (−) ─────┬───────► fan pin 1  (black, both fans)
                  └───────► LOLIN S3 GND
```

**The 12 V ground and the LOLIN ground must be tied together** or neither the
PWM nor the tach signal has a return path. During bring-up power the LOLIN from
USB-C and let the 12 V rail feed only the fans; that is safe, since the two
supplies only share ground. For standalone operation add a 12 V → 5 V buck into
the board's 5 V pin and leave USB unplugged.

### PWM out (GPIO 11, GPIO 12)

```
GPIO 11 ──[100 Ω]──► fan 1 pin 4 (blue)
GPIO 12 ──[100 Ω]──► fan 2 pin 4 (blue)
```

Noctua's PWM input is internally pulled up and is driven fine by 3.3 V
push-pull logic — this is the same arrangement a Raspberry Pi uses. If you want
strict Intel-spec 5 V levels, put a 74AHCT125 buffer on 5 V in the path; nothing
in the firmware changes.

### Tach in (GPIO 13, GPIO 14)

```
                  3V3
                   │
                 [10 kΩ]
                   │
fan pin 3 ─[1 kΩ]──┴──► GPIO 13   (fan 2 → GPIO 14)
   (green)
```

The tach line is an open-collector transistor to fan ground with no internal
pull-up, so it never presents 12 V — pulling it up to **3.3 V, never 5 V or
12 V**, is what keeps it in range for the S3.

Full fan 1 wiring on the board: `../docs/wiring/full-size-s3/fan-wiring.png` (source
`../docs/wiring/full-size-s3/fan-wiring.excalidraw`). Fan 2 is identical on `12` / `14`.

### Thermistors (GPIO 1, GPIO 2)

```
3V3 ──[10 kΩ 1%]──┬──► GPIO 1
                  │
                [NTC]        (optional: 100 nF from the node to GND — not fitted)
                  │
                 GND
```

The probe is a bare 2-wire NTC and has no polarity. Hotter probe → lower node
voltage. If the node sits at either rail the firmware calls it a fault (open or
shorted probe), reports `FAULT` and stops the fan that probe controls.

### Where these pins are on the board

Verified against a LOLIN S3 v1.0.0 (module marked `MCN16R8`). Hold the board
**component side up** — the side with the metal Espressif can and the two
buttons — with the **two USB-C sockets pointing down at you**:

| Signal | Pin | Where to find it |
|--------|-----|------------------|
| Thermistor 1 | `1`  | Right edge, 2nd hole from the top (top hole is `GND`) |
| Thermistor 2 | `2`  | Right edge, 3rd hole from the top |
| Fan 1 PWM | `11` | Left edge, 4th hole up from the bottom |
| Fan 2 PWM | `12` | Left edge, 3rd hole up from the bottom |
| Fan 1 tach | `13` | Right edge, first hole above the 3-hole `GND` block |
| Fan 2 tach | `14` | Right edge, second hole above that `GND` block |

The layout works in your favour: the two tach pins sit directly against a
3-hole ground block, the two PWM pins are next to `5V`/`GND` at the bottom of
the left edge, and the two thermistor pins are next to the top `GND`, directly
across the board from the pair of `3V3` pins (top of the **left** edge) that
feed their dividers. See `../docs/wiring/full-size-s3/probe-wiring.png` (editable source:
`../docs/wiring/full-size-s3/probe-wiring.excalidraw`).

### LOLIN S3 Mini

**Complete wiring, everything in one schematic:** `../docs/wiring/mini-complete-wiring.png`
(source `../docs/wiring/mini-complete-wiring.excalidraw`) — probes, both fans with
their pull-ups and pull-downs, the on/off switch, power, and the `0`
button/LED legend. Name tags (`3V3`, `GND`, `12 V`, `pin N`) with the same
name are one connection.

The same firmware and pins also run on a LOLIN S3 Mini v1.0.0 (build env
`lolin_s3_mini`, the default). Held chip up, USB socket at the bottom, every
signal is in the **left** block: outer column `EN 2 4 12 13 11 10 3V3`, inner
column `1 3 5 6 7 8 9 14`. `GND` is the 7th row of the right block (both
columns). The Mini has **one** `3V3` hole, so all four pull-up/divider
resistors share it through a breadboard rail. Probe wiring:
`../docs/wiring/mini-probe-wiring.png` (source `../docs/wiring/mini-probe-wiring.excalidraw`);
both fans: `../docs/wiring/mini-fan-wiring.png` (source `../docs/wiring/mini-fan-wiring.excalidraw`).

The Mini has only the native USB socket (no `UART` fallback). A brand-new
board, or one whose firmware has hung, is flashed by holding **`0`**, tapping
**`RST`**, releasing **`0`**.

### Why these pins and not others

`GPIO 1/2` are on ADC1, which — unlike ADC2 — keeps working while Wi-Fi is on,
so leave the thermistors there if you ever add networking. `GPIO 11–14` are
plain general-purpose pins with nothing else attached. Deliberately avoided:
`0/3/45/46` (strapping pins, sampled at boot), `19/20` (not broken out — the
OTG socket uses them), `26–37` (consumed by the 16 MB flash and 8 MB PSRAM),
`43/44` (the serial console), and `38` (the onboard RGB LED). All six
assignments are in one block at the top of `src/main.cpp`.

### The two USB-C sockets

`OTG` connects straight to the ESP32-S3 itself; `UART` goes through a CH340
serial chip (the `340XD28` part on the front). Both can flash the board and
both carry the console. The difference only matters when something goes wrong:
the `UART` socket can always reset the board into its loading mode on its own,
whereas the `OTG` socket depends on the firmware currently running cooperating.
If a future firmware ever hangs and `OTG` stops responding, move the cable to
`UART` rather than reaching for the buttons.

## Firmware behaviour

- 25 kHz PWM, 10-bit, per Intel's 4-wire spec.
- Fans boot stopped (no full-speed burst) and start on the first control tick
  if their probe is at or above 30 °C.
- The probes sit at the back of the fan shrouds, in air the two ASUS GX10s
  have already heated, so the curve is set for exhaust air (an idle GX10
  stays under 30 °C there; heavy load reads 45–55 °C):

| Probe | Duty |
|-------|------|
| < 30 °C | off |
| 30 °C | 20 % |
| 35 °C | 30 % |
| 40 °C | 45 % |
| 45 °C | 65 % |
| 50 °C | 85 % |
| ≥ 55 °C | 100 % |

- Below 30 °C a fan is **stopped** (0 % PWM — verified that these Noctuas
  report 0 RPM there); between the points duty is interpolated linearly.
  1–19 % is never commanded, since an NF-12 can
  stall or chatter there: a fan is either off or at `DUTY_MIN` (20 %) or more.
  A stopped fan restarts with a 50 % kick and then slews down to its target;
  once running, a fan only stops when its probe falls below 28.5 °C
  (30 °C minus the 1.5 °C hysteresis), wherever the temperature peaked.
- 1.5 °C of hysteresis on the way down and a 1 %-per-100 ms slew limit, so the
  fans don't hunt or surge.
- A probe reading outside 200–2800 mV is treated as open or shorted: it is
  reported as `FAULT` and **the fan that probe controls stops**; the other fan
  carries on. There is deliberately no full-speed failsafe. The bounds stay
  clear of where the S3's ADC stops tracking near 3.3 V, so an unplugged probe
  shows up as `FAULT` rather than as a plausible "very cold" reading.
- Each fan follows its own probe: fan 1 ← probe 1 (`t0`), fan 2 ← probe 2
  (`t1`) (`ZONE_MODE 1`). Set `ZONE_MODE 0` to have both fans track the
  *hotter* probe instead; there a faulted probe is ignored and both fans stop
  only if both probes are faulted.
- RPM is counted from the tach at 2 pulses/revolution and reported once a second,
  by the S3's hardware pulse counter with its glitch filter at the maximum
  (12.8 µs). Without the filter, spikes coupled from the adjacent 25 kHz PWM
  wire are counted as pulses and the RPM reads high at every duty except 100 %
  (it even climbed while the fans were slowing down).

### On/off switch, profile button and LED

- **On/off switch** — DFRobot DFR0789 Gravity LED Switch (self-locking) on
  GPIO 21: `D` → `21` (right block, inner column, 5th hole), `+` → `3V3`
  rail, `−` → `GND`. Pressed in (lit) = **fans on**, out = **fans off**,
  whatever the profile. Honoured at power-up. Drawing:
  `../docs/wiring/mini-switch-wiring.png` (source `../docs/wiring/mini-switch-wiring.excalidraw`).
- **`0` button** on the board (next to the USB socket) picks the profile:

| Press | Effect |
|-------|--------|
| single | Normal ↔ Quiet (from Max: → Normal) |
| double | Max ↔ Normal |
| long (≥ 1 s) | back to Normal |

- **LED** (the one marked `47`): **green** Normal, **blue** Quiet, **red**
  Max, **amber** switch off. (The Mini's LED has red and green swapped
  relative to what the core sends; the firmware corrects for it.)
- **Normal** — the curve above, capped at 80 % (≈ 2400 RPM). **Quiet** — the
  same curve scaled to 67 % (≈ 2000 RPM at the top). Both keep to their cap
  however hot it gets. **Max** — 100 % (≈ 3000 RPM), even with a faulted
  probe; the only profile that reaches full speed.
- With the switch off, a fan whose probe reaches **55 °C** runs on the
  current profile until it has cooled below 53.5 °C.
- The web page can also **Identify** a fan (that fan 100 %, the other
  stopped, 15 s, with a countdown and Stop) and **Turn off** each fan. Turn
  off is a hard stop that nothing overrides, heat included; a restart or
  flipping the switch turns the fans back on.
- Every power-up starts in Normal. Choosing a profile or flipping the switch
  ends any console `set`/`max` hold.

### Wi-Fi, web page and updates over the network

Optional; fan control never depends on it. Set up once over USB (type these in
`pio device monitor` yourself so the passwords never leave your machine):

```
wifi ssid <network name>
wifi pass <wifi password>
webpass <password for changes and updates>
```

Everything is kept in the board's flash. The board then joins the network on
every power-up (retrying every 30 s if it drops) and serves
**http://dgx-fans.local/**: both temperatures, fan speeds, the switch state and
Normal / Quiet / Max buttons. Viewing is open to the local network; the
buttons ask for user `fans` and the web password, and stay disabled until one
is set. The on/off switch still has the final say.

`wifi` shows the connection and IP, `wifi forget` erases the network.

Firmware updates over Wi-Fi use the same password, taken from the environment
so it never lands in `platformio.ini`:

```
DGX_OTA_PASS='…' ~/.platformio/penv/bin/pio run -e lolin_s3_mini_ota -t upload
```

Plain HTTP on the local network: the password is not encrypted in transit, so
keep the page off anything but your own network.

### Serial console

115200 baud over the native USB CDC port.

```
auto        return to curve control
max         both fans to 100 %
set <pct>   hold a fixed duty (0 = off; 1–19 is raised to 20)
profile <normal|quiet|max>   same as the button
wifi | wifi ssid <name> | wifi pass <pw> | wifi forget | webpass <pw>
curve       print the active curve
help
```

`max` and `set` outrank the sensor-fault stop, so the fans can be bench
tested before the probes are wired. A manual hold expires after 10 minutes
(`MANUAL_TIMEOUT_MS`) and reverts to curve control, so the rack cannot be left
parked at a fixed speed by accident.

Status lines look like:

```
t0=31.4 t1=29.8 duty=32%/32% rpm=712/705
```

with ` [manual]`, ` [quiet]` / ` [max]` and ` [switch off]` appended when active.

## Calibrating the Beta value

InLine publishes 10 kΩ @ 25 °C but not a B value; the firmware assumes the
common `NTC_BETA = 3950`. That is close enough to be useful out of the box and
worth correcting if you want better than ±2 °C:

1. Note the reported temperature next to a reference thermometer at room
   temperature, then again with the probe in an ice-water bath (0 °C) or held at
   body temperature.
2. Raise `NTC_BETA` if the firmware over-reports the spread from 25 °C, lower it
   if it under-reports.

Accuracy near 25 °C is set almost entirely by `NTC_R25` and the 1 % divider
resistor, which is why that resistor should not be a 5 % part.

## Build and flash

```
pio run -t upload -t monitor
```

The LOLIN S3 enumerates as `/dev/ttyACM0`. If the port doesn't appear, hold
**BOOT**, tap **RST**, release **BOOT** to force the ROM bootloader.
