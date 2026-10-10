# Changelog

[中文](CHANGELOG.md) · **English**

## v0.1.2

### 🤨 What's Changed

- Added playback progress sharing with two timeline formats: Original + Pitch / Adjusted. Original + Pitch is the default and shares the original timeline with the actual playback rate; Adjusted shares a speed-adjusted timeline with a playback rate of 1. Actual progress is polled continuously without republishing normal advancement. State or timeline changes trigger updates, and position drift above 100 ms triggers correction.
- Added playback position control through SMTC, preserving the playing or paused state.
- Added cover, track information, playback progress and playback position control switches. There are now five switches, arranged in this order: master enable, cover, information, progress and position control. All are enabled by default.
- Added progress log summaries approximately every 5 seconds, recording queries, publication attempts and update reasons.

## v0.1.1

### 🤨 What's Changed

- Set artwork limits to 5000 × 5000 pixels and 10 MB.
- Limited online artwork downloads to one redirect, with 1-second timeouts per stage and a 5-second overall download deadline.
- Added timeouts and shutdown handling for Windows asynchronous operations to prevent prolonged waits from delaying media controls or plugin shutdown.

## v0.1.0

- First public version.
- For detailed instructions, please check the [README](../../README_en.md).
