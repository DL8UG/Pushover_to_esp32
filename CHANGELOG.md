# Changelog

All notable changes to this project are documented in this file.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
versions follow [Semantic Versioning](https://semver.org/).

## [Unreleased]

## [1.0.0] - 2026-10-06

### Added

- Pushover Open Client over WebSocket: real-time delivery, messages with
  priority ≥ 1 stored (up to 30, kept over power loss), acknowledgement by
  long press.
- Alarm over the ES8311 codec with random pitch, length and pauses; mute
  by long press on both buttons; quiet warning beep after 15 minutes
  without a connection.
- Messages expire after 24 h without acknowledgement
  (`MESSAGE_EXPIRY_HOURS`, 0 = off); clock via SNTP.
- Up to three WiFi networks with automatic switching, connection watchdog.
- Display: home screen with status icons, clock, message count and
  "NOT CONNECTED" banner; message screen with header (position, emergency
  marker, local time) and auto-sized title. Proportional fonts with
  Latin-1 characters (umlauts).
- Display simulator (`make -C test/sim`) rendering all screens to PNG.
- Board drawing with button functions in the README, generated from the
  simulator output (`tools/board_svg.py`).
- Own e-paper driver for the SSD1681 (built-in waveforms) and supply
  rail set-up.
- Credentials in `main/secrets.h` (not committed), template
  `main/secrets.h.example`.
- CI: simulator, firmware build, documentation check; release workflow.

[Unreleased]: https://github.com/DL8UG/Pushover_to_esp32/compare/v1.0.0...HEAD
[1.0.0]: https://github.com/DL8UG/Pushover_to_esp32/releases/tag/v1.0.0
