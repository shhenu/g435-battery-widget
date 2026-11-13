# Changelog

## 0.2.0 — 2026-11-13

- Add the Realtek/HDT647 `046D:0ACB` bridge protocol alongside `046D:0ADF`.
- Support both 64-byte feature writes/raw PI replies and 515-byte output writes
  with counted fragments; fallback requires confirmed cleanup of the first session.
- Select transport by known USB-ID, HID usage page, and report sizes.
- Bound PI fragment parsing and remote-mode retries; always attempt bridge exit
  after an attempted entry, including cancellation and errors.
- Add read-only `--diagnose`, a tray diagnostics menu, and `Diagnose-G435.bat`.
- Report inaccessible or unsupported control interfaces distinctly.
- Retry discovery after a USB change when the dongle was previously missing.
- Add hardware-independent transport, packet, parser, and cleanup tests to CI.
- Preserve the 15-minute polling interval and idle worker/USB-handle cleanup.

The 0ADF path is tested on real Windows hardware. The 0ACB path has protocol and
failure-injection coverage; a real 0ACB dongle was not available for this release.

## 0.1.0 — 2026-11-13

- First public release for Windows x64 and the G435 046D:0ADF LIGHTSPEED dongle.
- Native C / Win32 tray icon with estimated battery percentage and millivolts.
- Battery read at startup, every 15 minutes, and on manual refresh.
- Single-instance tray app; USB access and worker thread exist only during reads.
- About 6 MB working set and 1 MB private committed memory in a local idle test.
- One-shot JSON output, refresh, and quit commands.
