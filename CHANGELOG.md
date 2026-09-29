# Changelog

The firmware and GX-RACK share one version number. The firmware reports its
version on its web page, in `/api/status` and with the console command
`version`; GX-RACK shows both versions at the bottom of the dashboard.

## 1.1.0 — 2026-09-29

- **One GX10 or two:** `fan_count` in `controller/platformio.ini` (1 or 2,
  default 2). With 1, only fan 1 / probe 1 are used; the web page, status
  data and console show a single fan.
- `/api/status` gains `"version"` and `"fans"`; its per-fan lists are as long
  as the number of fans.
- GX-RACK reads the fan count from the controller and sizes the dashboard,
  history and MCP tools to it (controllers without the field count as two).
- Version numbers: firmware (`FW_VERSION`) and GX-RACK (`VERSION`), shown on
  both pages; console command `version`.

## 1.0.0 — 2026-09-28

First public release: two fans, two probes, curve and profiles, on/off
switch, Wi-Fi page and OTA, GX-RACK dashboard with REST and MCP.
