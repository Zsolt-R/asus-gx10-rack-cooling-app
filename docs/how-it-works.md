# How the DGX rack fan controller works

## The idea

Two ASUS Ascent GX10s sit in the rack. Each one pulls air in through its
perforated bottom plate and blows it out of the back. Under each GX10 sits a
Noctua 120 mm fan in a shroud, blowing room air up into that bottom intake,
which helps the GX10's own fans. GX10 owners report 5–15 °C lower internal
temperatures with a fan like this. That matters: the GX10 shuts itself off
when its board sensor reaches about 94–96 °C.

A temperature probe is taped at the back of each fan's shroud. It sits in air
the GX10 has already heated, so it rises and falls with how hard that GX10
is working. An idle GX10 keeps it under 30 °C; heavy work puts it at 45–55 °C.

A small controller board (LOLIN S3 Mini) reads both probes ten times a
second and sets each fan's speed. **Fan 1 follows probe 1, fan 2 follows
probe 2** — each GX10 gets as much air as it needs, independently.

```
 room air ─► Noctua fan ─► GX10 bottom intake ─► GX10 ─► warm air out the back
                                                        │
                                          probe (back of the shroud)
                                                        │
                                   controller reads it, sets that fan's speed
```

## The temperature curve

| Probe | Fan speed | About |
|-------|-----------|-------|
| below 30 °C | off | 0 RPM |
| 30 °C | 20 % | 660 RPM |
| 35 °C | 30 % | 950 RPM |
| 40 °C | 45 % | 1400 RPM |
| 45 °C | 65 % | 2000 RPM |
| 50 °C | 85 % | 2550 RPM |
| 55 °C and up | 100 % | 3000 RPM |

Between the rows the speed is in proportion. Some details keep it calm:

- **No hunting.** A fan only slows down once its probe has cooled 1.5 °C, so
  it doesn't pulse up and down around a step.
- **No surges.** Speed changes by at most 1 % every tenth of a second
  (10 % per second).
- **Clean starts and stops.** Below 20 % these fans can stall, so a fan is
  either off or at 20 % or more. A stopped fan starts with a short 50 % kick,
  then settles. A running fan stops only when its probe drops below 28.5 °C.
- **Smoothing.** Probe readings are averaged a little so electrical noise
  doesn't move the fans.

## Profiles

The profile sets each fan's top speed. It never goes above that, however
hot it gets.

| Profile | Top speed | LED |
|---------|-----------|-----|
| **Normal** | curve, capped at 80 % (≈ 2400 RPM) | green |
| **Quiet** | curve × 67 % (≈ 2000 RPM at the top) | blue |
| **Max** | always 100 % (≈ 3000 RPM) | red |

The controller always starts in Normal after power-up.

## Controls

**On/off switch** (the lit push switch). Pressed in and lit = fans on. Out =
fans off, whatever the profile — except that a fan whose probe reaches 55 °C
runs anyway (on the current profile) until it has cooled below 53.5 °C. The
controller follows the switch position after a power cut. Flipping it also
turns back on any fan that was turned off on the web page.

**`0` button** on the board: single press Normal ↔ Quiet, double press Max,
hold for a second → Normal. The LED shows the result (amber = switch off).

**Web page** — `http://dgx-fans.local/` on the home network:

- both temperatures, each fan's speed and RPM, Wi-Fi signal, uptime;
- **Normal / Quiet / Max** buttons;
- **Identify fan 1 / 2** — that fan runs at 100 % and the other stops for
  15 seconds, so you can see and hear which is which. The button counts down
  ("Stop · 12 s") and stops it early when pressed;
- **Turn off fan 1 / 2** — stops that fan completely, immediately, and
  nothing restarts it — not heat, not Max, not Identify — until you press
  **Turn on**, flip the switch, or the controller restarts.

Looking is open to anyone on the network; pressing buttons asks for the user
name and web password.

**USB console** (115200 baud, when the board is on USB): `auto`, `max`,
`set <pct>`, `profile …`, `identify 1|2|stop`, `fan 1|2 on|off`, `curve`,
`wifi …`, `help`. A `set`/`max` hold ends by itself after 10 minutes.

## What wins when things disagree

For each fan, the first rule that applies decides:

1. Turned off on the web page → **off**.
2. Identify running → **100 %** for the fan being identified, **off** for the other.
3. Console `set`/`max` → that speed (ends after 10 minutes).
4. Max profile, switch on → **100 %**.
5. Probe broken or unplugged → **off**.
6. Otherwise the curve: off if the switch is off (unless ≥ 55 °C); Quiet
   × 67 %; Normal capped at 80 %.

## Safety choices (made on purpose)

- **A broken probe stops its fan**, it does not run it at full speed. The
  controller calls a probe broken when its reading is outside the range a
  real temperature can give (open wire, short). The web page then shows
  `FAULT`.
- **Fans start stopped** at power-up — no full-speed burst.
- **Turn off means off**, even when hot.
- **With the controller unplugged the fans stop.** Noctuas run flat out when
  their speed wire is left floating; a 1 kΩ resistor from each fan's blue
  wire to ground holds it low instead.
- **The network never holds up the fans.** If Wi-Fi drops, the page is just
  unreachable and fan control carries on; the controller rejoins by itself
  every 30 seconds.

## Measuring the fans

Each fan's green wire gives two pulses per turn. The board counts them with
its hardware pulse counter, filtered so that interference from the speed wire
next to it is ignored (without the filter, RPM read up to 50 % high). RPM is
worked out once a second.

## Updating the firmware

Over Wi-Fi, from the PC: `agent/ota-upload.sh` (see `../agent/AGENT.md`).
A Wi-Fi update needs the controller to connect back to the PC. If your
router blocks that (for example, the controller on a separate 2.4 GHz or IoT
network), the script briefly moves the PC's Wi-Fi to the controller's
network for the upload and puts it back.

## Is the curve right?

The real limit is inside the GX10 (its board sensor, "acpitz"). Run a long,
heavy job and watch it: staying below about 85 °C means the curve is fine;
climbing toward 90 °C means the fans should ramp up earlier — lower the
temperatures in the curve (`CURVE[]` in `controller/src/main.cpp`).
