# Development

[中文](DEVELOPMENT.md) · **English**

## Build prerequisites

The projects require Windows x64, Visual Studio's Desktop development with C++ components, MSVC v145, and the Windows SDK. They use C++20. Install TagLib and zlib as static dependencies through vcpkg:

```powershell
.\vcpkg.exe install taglib:x64-windows-static
```

Set `VCPKG_ROOT` to the vcpkg root directory and reopen Visual Studio. The projects use `installed/x64-windows-static`; use `TagLibRoot` to specify a manually built installation. Release uses /MT and Debug uses /MTd. Dependencies must match the architecture and runtime library.

Open [SMTC.vcxproj](../../Plugin/SMTC/SMTC.vcxproj), select **Release / x64**, and build. The DLL is generated at `artifacts/x64/Release/SMTC.dll`. SDK headers and dependency settings are included in the project.

## Source structure

Plugin code is under `Plugin/SMTC`. Official VirtualDJ SDK headers are included in this directory.

| File | Responsibility |
| --- | --- |
| [SMTC.cpp](../../Plugin/SMTC/SMTC.cpp) | Plugin entry point, built-in settings, lifecycle, Windows SMTC session, and button events |
| [BridgeCore.h](../../Plugin/SMTC/BridgeCore.h) | Deck mapping, polling intervals, track identity, and control command validation |
| [Cover.h](../../Plugin/SMTC/Cover.h) / [Cover.cpp](../../Plugin/SMTC/Cover.cpp) | Asynchronous artwork tasks, embedded tags, main database lookup, and downloads |
| [Logger.h](../../Plugin/SMTC/Logger.h) | Log formatting, synchronized writes, and file size management |
| [SMTC.rc](../../Plugin/SMTC/SMTC.rc) / [SMTC.def](../../Plugin/SMTC/SMTC.def) | DLL version resources and exports |

## Plugin interface and SMTC session

The plugin uses VirtualDJ's Basic interface and the host's query and command APIs. Settings appear in VirtualDJ's built-in parameter interface. There is no separate settings panel, DSP interface, or StartStop interface. The internal Enable SMTC parameter starts and stops the bridge.

The bridge thread initializes a multithreaded COM apartment, creates a hidden top-level window, and obtains its SMTC object through `ISystemMediaTransportControlsInterop::GetForWindow`. The same thread owns the window and message pump. `DisplayUpdater` publishes the title, artist, album, and artwork; `PlaybackStatus` publishes the playing or paused state. The implementation does not publish a playback timeline.

Shutdown closes the command mailbox and signals the thread, cancels artwork work, removes the button subscription, closes the SMTC session, and destroys the window. Windows callbacks capture the shared mailbox instead of the plugin object, avoiding direct access to a released plugin instance.

## State queries and metadata updates

VirtualDJ state is polled at ten intervals from 100 to 1000 ms, with a default of 100 ms. The bridge uses `MsgWaitForMultipleObjects` to wait for stop signals, wake signals, polling timeouts, and window messages. Media buttons and artwork completion wake the thread without waiting for the polling timeout.

Each query resolves the source Deck, then reads `loaded`, `play`, and `get_filepath`. Track identity includes the resolved Deck number, loaded state, confirmed empty state, path, and settings revision. If identity is unchanged, the title, artist, and album are reused to reduce string queries and repeated publication.

When identity changes, the plugin reads `get_title_remix` first to retain version text such as Extended Mix. It falls back to `get_title`, `get_filename`, and a Deck placeholder title. After reading metadata, it checks the loaded state and path again, discarding a snapshot if the track changed during the query. Host queries are not transactional; this check reduces the chance of combining metadata from different tracks.

Query failures are distinguished from a confirmed empty Deck. If a published track becomes temporarily unavailable, its information is retained for up to two seconds while controls are suspended and artwork work is cancelled. After the timeout, the display becomes `No track loaded` while the SMTC object remains alive. A confirmed empty Deck can still receive playback and track navigation commands.

## Deck sources and playback commands

Fixed sources use Deck 1-4 directly. Left and Right resolve their actual numbers through `deck left get_deck` and `deck right get_deck`; Master uses `get_activedeck`. Subsequent queries and commands use the resolved number so that a left/right mapping change during processing does not redirect a command.

The SMTC `ButtonPressed` callback queues only the action, the displayed track snapshot, and the current Windows session source. The mailbox holds at most 32 requests. Before executing a request, the bridge rechecks the enabled state, settings revision, and track identity. Stale requests are discarded.

| Media action | VirtualDJ command |
| --- | --- |
| Play | `deck N play` |
| Pause | `deck N pause` |
| Next | `deck N load_next keepplay` |
| Previous | `deck N load_previous keepplay` |

N is the resolved Deck number. Previous and next navigate VirtualDJ's current browser track list; `keepplay` preserves playback state. Confirmed empty Decks use the same commands.

Track navigation also checks the current Windows media session. It compares the application source captured by the callback with the source at execution, then matches the published title, artist, and album. Play and pause bypass this session filter. If the session manager is unavailable, the plugin relies on its own identity checks. These checks do not change how Windows selects a media session.

## Artwork extraction and asynchronous publication

Artwork runs on a separate worker so tag reads and downloads do not block state queries. Each track identity change clears the previous artwork and submits one task. Local files use embedded images read through TagLib, preferring Front Cover. The plugin does not search the database or neighboring image files for local tracks.

For `netsearch://` paths, `get_vdj_folder` locates the main `database.xml`. The XML scan matches a Song's FilePath, or a Link's NetSearch attribute against the track identifier with the prefix removed. It reads the matching Link's Cover attribute and downloads the image through WinHTTP, allowing at most one redirect. Resolve, connect, send, and receive timeouts are each set to one second, and asynchronous waits share a five-second deadline. Missing links and failed downloads leave the track without artwork.

XML is read as a stream with DTD processing prohibited and limits on file size, nesting depth, and scan time. WIC validates images. Current limits are 10 MB (10,000,000 bytes), with width and height each capped at 5000 pixels. Oversized images are rejected rather than resized.

Each task has an increasing ticket number. Track changes and disabling invalidate old work. The worker checks cancellation, and the publisher checks the ticket again. Valid image bytes are written to an `InMemoryRandomAccessStream` and supplied as an SMTC thumbnail through `RandomAccessStreamReference`. Bytes are retained only by objects needed for current processing and display; there is no historical artwork or URL cache.

Each track visit gets one artwork attempt. Failed or cancelled tasks are not automatically resumed. A new track visit or re-enabling the plugin starts another attempt.

WinRT asynchronous operations on the bridge thread use bounded waits of up to one second, checking the stop event at intervals of no more than 25 ms. GetResults is called only after successful completion. Timeout or shutdown requests cancellation without waiting for completion. An artwork write timeout drops that publication; a media-session validation timeout drops the navigation request; a session-manager initialization timeout uses the existing fallback. The artwork completion callback retains only the stream and writer, without accessing the plugin, so late completion does not use released resources.

## Error handling and logging

A failed state update logs the error, clears published state, and suspends command acceptance. Later polls continue attempting updates. If the bridge thread exits entirely, the settings interface requests re-enabling. Re-enabling joins the exited thread and starts a replacement; there is no automatic restart loop. A failed artwork worker is restarted by the next artwork request.

`Logger.h` serializes writes to `SMTC.log` beside the DLL. Records include a UTC timestamp, level, category, process and thread IDs, and English content. If the next record would exceed 10 MiB, the file is cleared before writing. Open or write failures disable file logging without falling back to another directory. Logging failures do not block SMTC functionality.
