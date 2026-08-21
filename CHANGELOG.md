# Changelog

Notable user-visible changes are recorded here. Package revisions must be
upgraded as a matching `nes-emulator` and `luci-app-nes-emulator` pair.

## Unreleased

- Added a fail-closed OpenWrt installer that detects the router's exact package
  ABI, selects the latest matching GitHub release, verifies both package
  checksums, and installs the native daemon and LuCI application together.

## 1.0.0-r19 — initial public release

- Router-side FCEUmm emulation with raw RGB565 or software JPEG streaming.
- LuCI management, authenticated ROM import, UCI/procd integration, and safe
  on-demand startup.
- Custom keyboard controls, standard browser gamepad input, optional touch
  controls, fullscreen 4:3/16:9 display, and FCEUX-style canvas FPS OSD.
- Ten integrity-checked full-machine save-state slots per ROM and asynchronous
  atomic SRAM persistence.
- Bounded audio/video queues, bidirectional heartbeat leases, reconnect
  recovery, nonblocking HTTP responses, and asynchronous latest-frame JPEG
  encoding.
- Static APK v3 packages for 35 OpenWrt 25.12 ABI values, plus exact
  corresponding source and checksums.

Internal package revisions before r19 were development snapshots and are not
intended to be mixed with this build.
