# Fasting Tracker for Flipper Zero

A Flipper Zero app for tracking an intermittent fasting schedule.

## Features

- Live fasting timer with a progress bar towards your goal
- Plans: 13:11, 16:8, 18:6, 20:4, OMAD (23:1), 36h and 48h
- Eating-window countdown after you break your fast
- Vibration/LED/sound alert when you hit your goal (while the app is open)
- Adjust the start time if you forgot to press Start
- History of your last 30 fasts, with average duration and goals hit
- State is saved to the SD card, so the timer keeps running across app exits and reboots (it uses the Flipper's real-time clock)

## Controls

**Main screen**

| Button | Action |
| --- | --- |
| OK | Start a fast / end the current fast (asks to confirm) |
| Left | Cycle through fasting plans |
| Right | Open history |
| Up / Down | While fasting: move start time 15 min earlier / later (hold to repeat) |
| Back | Exit (the fast keeps going) |

**History**

| Button | Action |
| --- | --- |
| Up / Down | Select an entry |
| Hold OK | Delete the selected entry |
| Back / Left | Return to the main screen |

A ✓ next to an entry means that fast met its goal. Fasts shorter than a minute are not logged.

## Building

Install [ufbt](https://github.com/flipperdevices/flipperzero-ufbt), then from this directory:

```bash
ufbt
```

To build and launch on a Flipper connected over USB:

```bash
ufbt launch
```

The compiled `.fap` ends up in `dist/`. Copy it to `SD Card/apps/Tools/` with qFlipper if you're not using `ufbt launch`. CI also builds the app on every push; download the `.fap` from the workflow run's artifacts.

Make sure the Flipper's clock is set correctly (qFlipper syncs it when you connect).
