# Changelog

## 0.1.0 — 2026-11-13

- First public release for Windows x64 and the G435 046D:0ADF LIGHTSPEED dongle.
- Native C / Win32 tray icon with estimated battery percentage and millivolts.
- Battery read at startup, every 15 minutes, and on manual refresh.
- Single-instance tray app; USB access and worker thread exist only during reads.
- About 6 MB working set and 1 MB private committed memory in a local idle test.
- One-shot JSON output, refresh, and quit commands.
