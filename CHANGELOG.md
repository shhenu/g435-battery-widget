# Changelog

## 0.3.0 — 2026-11-13

- Distribute a standalone Windows x64 executable with embedded MIT notices.
- Automatically select both known G435 LIGHTSPEED protocols without settings.
- Recognize compatible alternative USB IDs only when the Logitech G435/G435SE
  product identity and an implemented HID control-interface fingerprint match.
- Try the next supported receiver when the first is unavailable or unresponsive;
  stop on cancellation or unconfirmed session cleanup.
- Retry unavailable headsets after 5, 15, then 60 seconds; retain the 15-minute
  interval after a successful read and pause automatic polling on cleanup failure.
- Refresh after Logitech HID arrival/removal and Windows resume, with debouncing
  and no follow-up read for successful requests' own device notifications.
- Accept slower CRC64 battery replies, match the outstanding request sequence,
  and finish immediately after a valid answer.
- Attempt CRC64 session exit even if the read following an entry write fails.
- Add model-matching, receiver-failover, retry-policy, and sequence tests.
- Confirm a real 0ADF read: 4035 mV / estimated 84%; the idle widget remains about
  6 MB working set and 1.05 MB private memory, with one thread and a 30 KiB file.

0ACB remains covered by recorded packets and failure-injection tests. No physical
0ACB dongle was available; future incompatible protocols and Bluetooth are outside
this release's supported scope.

## 0.2.1 — 2026-11-13

- Fix false 0ADF battery timeouts when opaque PI header counters are nonzero.
- Parse and validate the complete OTA reply instead of requiring a zero byte
  immediately before its length field.
- Add the captured failing 4122 mV reply and header, padding, truncation, status,
  and sequence regression checks.
- Confirm the fix with a real headset reading: 4125 mV / estimated 93%.

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
