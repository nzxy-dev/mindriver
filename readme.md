# Mindriver

Background audio player written in C, CLI-controlled via IPC. Designed as a native binary with no heavy external dependencies. Runs as a background service when a file is played and handles subsequent calls from other terminals to control playback—ideal for engine-less game development or other interactive applications.

## Modules

The project is divided into modular layers, mirror-structured for both Windows and Linux:

### initcore
Handles audio engine startup and verification before processing any audio streams.
- **On Windows:** Initializes WASAPI in shared mode (`AUDCLNT_SHAREMODE_SHARED`). Makes a test call to the default audio endpoint to verify the mix format (sample rate, channels, and bit depth).
- **On Linux:** Uses ALSA exclusively for audio playback. At startup, it tries to open the default ALSA playback device; if unavailable, it displays a GTK alert with the message `Alsa not found install it` (or prints the message in the terminal if no graphical session is available). Each active playback runs on a detached POSIX thread and is tracked by the service for status and playback controls.

> [!IMPORTANT]
> In linuxs alsa just cant play `.WAV`audio files.

### resolver
Resolves relative and absolute paths, sanitizes filenames to generate thread identifiers, and decodes audio tracks.
The current resolver is basic and non-robust, though functional for direct and relative paths. It is recommended to avoid non-ASCII characters (no Cyrillic, Japanese, `ñ`, or accented characters) and special symbols in file paths or names to prevent resolution or loading failures.
- **Filename Sanitization:** Extracts the base filename, strips the extension, and strips out special characters, spaces, dots, or symbols. Retains only lowercase alphanumeric characters.
  Examples:
  - `shootblast.mp3` -> `shootblast`
  - `shoot blast.mp3` -> `shootblast`
  - `shoot_b.l#as$t.mp3` -> `shootblast`
  - `dayum.wac` -> `dayum`
- **Decoding:**
  - Includes a direct WAV PCM reader (8, 16, 24, 32-bit, and float) with no external libraries required.
  - On Windows using MSVC, it integrates Windows Media Foundation (`IMFSourceReader`) support to decompress MP3, AAC, and other formats directly into float arrays.

### servicefunc
Independent audio thread manager. Each played file runs on its own thread and its own WASAPI/POSIX stream.
Maintains a critical section / mutex-protected array storing the state of each thread:
- **loop:** `notloop` (0) or `loop` (1)
- **end action:**
  - `stopatend`: Stops playback when the track finishes, but keeps the thread alive in a paused state to allow subsequent replaying.
  - `deleteatend`: Destroys the thread and frees memory as soon as playback finishes.
  - `loopatend1` through `loopatend20`: Once the track finishes, waits for N seconds and then automatically switches to continuous looping.
- **playing flag:** `true` or `false` depending on the current playback state.

### main and IPC layer
The entry point detects whether an instance of `mindriver` is already running:
- If the named pipe (`\\.\pipe\mindriver_ipc` on Windows or `/tmp/mindriver.sock` on Linux) responds, the command is serialized and sent to the server process, displaying the response and exiting immediately.
- If no active server exists:
  - Control commands (such as `--stop` or `--status`) warn that no server is running.
  - If a file path is passed, the current instance boots up as the server, initializes the audio engine, spins up the IPC thread, and starts playing the requested file.

## Command Syntax

**Play audio:**
- `mindriver path/file.wav`
- `mindriver path/file.mp3 --loop`
- `mindriver path/file.wav --deleteatend`
- `mindriver path/file.wav --loopatend5`

**Control an existing thread:**
- `mindriver id --stop`
- `mindriver id --replay`
- `mindriver id --delete`
- `mindriver id --status`

**Check general status of all threads:**
- `mindriver --status`

**Shut down the service:**
- `mindriver --exit`

**Status output format:**
- `file.wav , notloop , stopatend , playing=true`
- `music.mp3 , loop , stopatend , playing=true`

---

This is an open-source project and its commercial use, along with derived copies or modifications, is strictly prohibited under the GPLv3 license.
