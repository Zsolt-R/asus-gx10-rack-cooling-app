# DGX rack fan controller — docs

| File | What |
|------|------|
| [how-it-works.md](how-it-works.md) | what the controller does, the curve, profiles, controls, safety choices |
| [components.md](components.md) | every part used, resistor values and colours, pins |
| [wiring/](wiring/) | wiring drawings: `.excalidraw` (editable) + `.png` |

## Wiring drawings (LOLIN S3 Mini — the board in the rack)

| Drawing | Shows |
|---------|-------|
| `wiring/mini-complete-wiring.png` | **everything in one schematic** — start here |
| `wiring/mini-probe-wiring.png` | both probes, laid out on the board |
| `wiring/mini-fan-wiring.png` | both fans, with pull-ups, pull-downs and 12 V |
| `wiring/mini-switch-wiring.png` | on/off switch, `0` button and LED |

`wiring/full-size-s3/` holds the older drawings for the full-size LOLIN S3.
They predate the 1 kΩ pull-downs on the fans' blue wires — add those if you
ever build that version.

To change a drawing: open the `.excalidraw` file at excalidraw.com (or edit
its text in any editor), save, then regenerate the pictures:

```
python3 docs/wiring/render.py            # all drawings
python3 docs/wiring/render.py docs/wiring/mini-fan-wiring.excalidraw
```

(needs Google Chrome or Chromium installed).

Dashboard, history and the MCP server for agents: `../gx-rack/README.md`
(optional, runs in Docker).

Firmware details for developers: `../controller/README.md`.
For an AI agent making changes: `../agent/AGENT.md`.
