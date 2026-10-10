<!-- Layout source: https://github.com/SmallM1NG/NCM-Auto-Dump-Tool/blob/HEAD/README.md -->

<p align="center">
  <img width="72%" alt="SMTC Plugin for VirtualDJ" src="Docs/imgs/title.png">
</p>

<p align="center"><b>SMTC-Plugin-for-VirtualDJ</b></p>
<p align="center">
	<a href="https://github.com/SmallM1NG/SMTC-Plugin-for-VirtualDJ/releases/latest"><img alt="Latest release" src="https://img.shields.io/github/v/release/SmallM1NG/SMTC-Plugin-for-VirtualDJ?color=brightgreen&label=Latest&style=for-the-badge"></a>
	<img alt="C++20" src="https://img.shields.io/badge/C%2B%2B-20-00599C.svg?logo=cplusplus&logoColor=white&style=for-the-badge">
	<img alt="Windows x64" src="https://img.shields.io/badge/Windows-x64-0078D6.svg?logo=windows&logoColor=white&style=for-the-badge">
	<img alt="VirtualDJ 8" src="https://img.shields.io/badge/VirtualDJ-8-E60000.svg?style=for-the-badge">
	<a href="LICENSE"><img alt="License: GPLv3" src="https://img.shields.io/badge/License-GPLv3-red.svg?style=for-the-badge"></a>
	<a href="https://github.com/SmallM1NG/SMTC-Plugin-for-VirtualDJ/stargazers"><img alt="Stars" src="https://img.shields.io/github/stars/SmallM1NG/SMTC-Plugin-for-VirtualDJ?style=for-the-badge"></a>
</p>

<p align="center"><a href="README.md">中文</a> · <b>English</b></p>

<p align="center"><a href="#project-introduction">Project Introduction</a> · <a href="#feature-showcase">Feature Showcase</a> · <a href="#how-to-install">How to Install</a> · <a href="#how-to-use">How to Use</a> · <a href="#additional-information">Additional Information</a> · <a href="Docs/md/DEVELOPMENT_en.md">Development</a> · <a href="Docs/md/CHANGELOG_en.md">Changelog</a></p>

---

<a id="project-introduction"></a>
## Project Introduction ℹ️

This plugin connects your VirtualDJ to Windows SMTC (System Media Transport Controls).

It shares the current track's title, artist, album, artwork, playback state, position and duration with Windows. System media buttons can control play, pause, previous track and next track, and applications supporting position control can request a seek. SMTC-compatible applications, such as Wallpaper Engine, can access this information or send playback commands.

The plugin uses VirtualDJ's built-in settings interface and lets you choose Deck 1-4, Left, Right or Master as the source for track information and playback control.

Local artwork is read from embedded file tags. Artwork for online tracks is retrieved using cover links in VirtualDJ's main database.

> This project is open source under [GPLv3](LICENSE). Please comply with the license when using, modifying or distributing it.

<a id="feature-showcase"></a>
## Feature Showcase ✨

<div align="center"><img width="1024" src="Docs/imgs/demo.webp" alt="VirtualDJ SMTC demo"></div>

---

<a id="how-to-install"></a>
## How to Install 📥

This plugin supports only **VirtualDJ 2021 and later on Windows x64** and requires a **VirtualDJ Pro** license.

<ol>
<li>

Download the **dll** file from the latest [release](https://github.com/SmallM1NG/SMTC-Plugin-for-VirtualDJ/releases/latest) (currently [v0.1.2](https://github.com/SmallM1NG/SMTC-Plugin-for-VirtualDJ/releases/tag/v0.1.2)).

</li>
<li>

Place **SMTC.dll** in the **Plugins64\AutoStart** folder inside your VirtualDJ data directory.

Versions before 2023 typically use `C:\Users\YourUsername\Documents\VirtualDJ\Plugins64\AutoStart`.  
Versions after 2023 typically use `C:\Users\YourUsername\AppData\Local\VirtualDJ\Plugins64\AutoStart`.  
If you changed the data directory, use your custom data directory.

</li>
<li>

Start VirtualDJ. Installation is successful when **SMTC** appears in the Master effects list.

</li>
</ol>

---

<a id="how-to-use"></a>
## How to Use ▶️

### 1. Automatic Startup

The plugin starts automatically with VirtualDJ by default. No manual activation is required.

### 2. Adjust Settings

To adjust the source or other settings, find **SMTC** in the Master effects list and click the small gear icon next to it to open the built-in settings page.

<p align="center">
  <img src="Docs/imgs/settings.png" alt="SMTC settings entry reference">
</p>

The following five switches default to enabled. VirtualDJ restores previously saved settings.

- **Enable SMTC**: Enable or disable the plugin's functionality.
- **Share Cover**: Share artwork, enabled by default. Disabling clears artwork and cancels retrieval; re-enabling retrieves artwork for the current track.
- **Share Track Info**: Share the title, artist and album, enabled by default. Disabling clears these fields and stops text tag queries; re-enabling reads the current track.
- **Share Progress**: Enable or disable playback progress sharing, enabled by default. Position and duration follow Timeline Mode. Enable Enable Seeking to also allow position changes.
- **Enable Seeking**: Allow seeking through SMTC, enabled by default. The short name in compact interfaces is Seeking. Requires Share Progress. Seeking preserves the playing or paused state.
- **Timeline Mode**: Select the progress time base, defaulting to Original + Pitch. Original + Pitch shares the original position and duration with the actual playback rate, matching lyric timestamps. Adjusted shares speed-adjusted position and duration with a playback rate of 1.
- **Source Deck**: Select Deck 1-4, Left, Right or Master as the source for track information and playback control.
- **Polling Interval**: Set how often track information and playback state are queried, at 100-1000 ms in 100 ms steps (10 options). The default is 100 ms. Shorter intervals provide faster updates but increase the number of queries.

Settings are saved automatically when VirtualDJ closes.

> To disable the plugin's functionality, use the disable button in the plugin settings. Do not disable the plugin using VirtualDJ's small green indicator. Doing so will hide the plugin from the list, and you will need to restart VirtualDJ to restore it.

---

<a id="additional-information"></a>
## Additional Information 📚

<p align="center"><a href="Docs/md/DEVELOPMENT_en.md">Development</a> · <a href="Docs/md/CHANGELOG_en.md">Changelog</a></p>

### Additional Notes 💡

#### 1. Why do some tracks have no artwork?

Local tracks use only artwork embedded in file tags. Online tracks require a valid cover link in VirtualDJ's main database, which the plugin uses to download the artwork automatically.

If no artwork is found, a download fails or an image exceeds the limits, the plugin leaves the artwork empty without periodic retries. Switching away and back, re-enabling the plugin or re-enabling Share Cover triggers another attempt.

#### 2. Why do edited track tags not update immediately?

The title, artist and album are read only when the track changes, the source changes, the plugin is re-enabled or Share Track Info is re-enabled.

#### 3. Which list do previous and next follow?

Track navigation follows the current song list in the VirtualDJ browser. VirtualDJ determines what happens at list boundaries and whether a playing track can be replaced.

#### 4. How is playback progress shared and controlled?

With Share Progress enabled, the plugin polls actual progress while receivers advance from the published anchor and rate. State or timeline changes are published immediately; position drift above 100 ms triggers correction. Polling Interval controls how often queries run; the default is 100 ms. Timeline Mode defaults to Original + Pitch, sharing the original media timeline and actual playback rate. Adjusted shares position and duration adjusted for the speed in VirtualDJ, with a playback rate of 1.

For a 300-second track played at 1.25 times speed, Original + Pitch publishes a duration of 300 seconds and a rate of 1.25; Adjusted publishes 240 seconds and a rate of 1. How lyric applications extrapolate position using the rate depends on their implementation.

With Enable Seeking also enabled, compatible applications can request a seek through SMTC. In either mode, the plugin seeks by the ratio of the requested position to the published duration, without applying the playback rate again and preserves the playing or paused state. Disabling position control leaves progress sharing available; disabling progress sharing also prevents seeking.

The application reading SMTC decides whether to show a progress bar and how clicks or dragging behave. Empty Decks, unknown duration and reverse playback do not expose progress or seeking.

#### 5. Where can I find the log?

Logs are saved as **SMTC.log** in the same folder as the DLL.

Logs are written in English and include UTC timestamps, entry types and detailed messages. While valid progress is available, a summary approximately every 5 seconds records valid queries, publication attempts, update reasons, mode, position, duration and playback rate. Reason counts may overlap. Once the log exceeds 10 MB, it is cleared and writing starts again.

---

### Bug Reports 😨

Please describe the issue in detail: what happens, whether it can be reproduced, and provide the SMTC plugin version, Windows version, steps to reproduce, relevant screenshots and the log file.

---

### Acknowledgements 🙌

The friends who tested this project

---

### LINK 🔗

<p align="center">
  <a href="https://space.bilibili.com/475951038">BILIBILI</a>
  ·
  <a href="https://www.virtualdj.com/wiki/Developers.html">VirtualDJ Developer Documentation</a>
</p>

---

### Donate 🧋

<p align="center">
  🥰Treat me to bubble tea, meow! Thank you, meow!🥰
</p>

<p align="center">
  <img width="420" src="Docs/imgs/qrcode.jpg" alt="Treat me to bubble tea">
</p>
