# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [1.1] - 2026-11-18

### Added

- Start time picker shown when starting a fast. It defaults to now; Up/Down move it in 15-minute steps (snapping to the quarter hour) and show when the goal will be reached.
- End time picker shown when ending a fast, so a fast you forgot to stop can be backdated. Shows the resulting fast length and whether the goal was met.

### Changed

- Start times can go back at most 7 days and never before the previous fast ended. This also applies to the Up/Down adjustment while a fast is running.
- End times must be at least one minute after the start, so the fast is always logged.

## [1.0] - 2026-11-18

### Added

- Fasting timer with a progress bar and time remaining to goal.
- Fasting plans: 13:11, 16:8, 18:6, 20:4, OMAD (23:1), 36h and 48h.
- Eating-window countdown after a fast ends.
- Vibration, LED and sound alert when the goal is reached while the app is open.
- Up/Down adjustment of the start time while a fast is running.
- History of the last 30 fasts with average duration and goals met; hold OK to delete an entry.
- State saved to the SD card, so a fast continues across app exits and reboots.
- GitHub Actions build against the release and dev SDKs.

[Unreleased]: https://github.com/daniebker/flipper_fasting/compare/v1.1...HEAD
[1.1]: https://github.com/daniebker/flipper_fasting/compare/v1.0...v1.1
[1.0]: https://github.com/daniebker/flipper_fasting/releases/tag/v1.0
