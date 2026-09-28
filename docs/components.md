# Components

What the controller as built is made of (LOLIN S3 Mini version, the one in
the rack). Wiring: `wiring/mini-complete-wiring.png`.

## Parts

| Qty | Part | Notes |
|-----|------|-------|
| 1 | **LOLIN S3 Mini v1.0.0** (ESP32-S3FH4R2, 4 MB flash, 2 MB PSRAM) | the controller; Wi-Fi built in, one USB-C socket |
| 2 | **Noctua 120 mm 4-pin PWM fan** | 12 V, 25 kHz PWM, 2 tach pulses/turn, ~3000 RPM max, stalls below ~20 % |
| 2 | **InLine 36219I temperature probe** | plain 10 kΩ NTC thermistor at 25 °C, 2 wires, −50…+90 °C |
| 1 | **DFRobot DFR0789** Gravity LED switch | self-locking push switch with LED, the on/off switch |
| 1 | **12 V DC adapter**, ≥ 1 A | feeds only the fans' yellow wires (two fans draw well under 0.5 A) |
| 1 | USB-C cable + USB power | powers the S3 Mini |
| 1 | breadboard / rail strip | the Mini has one `3V3` hole: all four resistors to 3V3 share it via a rail |

## Resistors (per fan / probe, ×2 each)

| Qty | Value | Colour bands | Where | Why |
|-----|-------|--------------|-------|-----|
| 2 | 10 kΩ **1 %** metal film | brown-black-black-red-brown | probe pin (1 / 2) → 3V3 | top half of the probe's voltage divider; 1 % keeps readings accurate |
| 2 | 10 kΩ | brown-black-orange | tach pin (13 / 14) → 3V3 | pull-up: the fan's speed signal only pulls down |
| 2 | 1 kΩ | brown-black-red | fan green wire → tach pin | protects the pin |
| 2 | 100 Ω | brown-black-brown | PWM pin (11 / 12) → fan blue wire | softens the speed-control edges |
| 2 | 1 kΩ | brown-black-red | fan blue wire (fan side of the 100 Ω) → GND | pull-down: fans **stop** when the controller is off, instead of running flat out |

(Four-band codes shown; a 1 % part usually has five bands.)

## Pins used

| Pin | Connects to |
|-----|-------------|
| `1` | probe 1 |
| `2` | probe 2 |
| `11` | fan 1 speed control (blue, via 100 Ω) |
| `12` | fan 2 speed control (blue, via 100 Ω) |
| `13` | fan 1 speed signal (green, via 1 kΩ) |
| `14` | fan 2 speed signal (green, via 1 kΩ) |
| `21` | on/off switch `D` |
| `3V3` | rail: probe resistors, tach pull-ups, switch `+` |
| `GND` | rail: probes, switch `−`, fan black wires, 12 V adapter `−` |
| `0` button, LED `47` | on the board: profile button, profile LED |

The 12 V adapter's `−` and the board's `GND` must be joined, or neither the
speed control nor the speed signal works. 12 V never goes to any hole of the
board.

## Not fitted (optional extras)

| Part | Why you might add it |
|------|----------------------|
| 2 × 100 nF ceramic capacitor, probe pin → GND | extra filtering of probe noise. In the original plan, not fitted (confirmed 2026-09-25): the firmware smooths the readings and they are steady without. |
| 12 V → 5 V buck converter into the board's `5V`/`VBUS` | run the controller from the 12 V adapter instead of USB |

## What it cools

| Qty | Device | Relevant facts |
|-----|--------|----------------|
| 2 | **ASUS Ascent GX10** (NVIDIA GB10) | air in through the bottom, out the back; up to 240 W each; rated for 0–35 °C room temperature; shuts off at ~94–96 °C board temperature |

## Wiring drawings

See [`wiring/`](wiring/) — start with
[`mini-complete-wiring.png`](wiring/mini-complete-wiring.png).
