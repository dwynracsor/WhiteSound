# White Sound (Bluetooth Keep Alive)

Emits very low level noise through the selected audio device to stop Bluetooth
speakers from putting their amplifier to sleep on silence. The application lives
in the system tray, with a native backend on Windows, macOS and Linux.

## Features

- **System tray application** (native tray on Windows, macOS and Linux) with a
  compact dark flyout with rounded corners
- **Two-step close**: closing the panel hides it and leaves the app running in
  the tray (on Windows it also drops off the taskbar); a second close exits
- **State at a glance**: the tray icon itself changes — a lit cyan/blue badge with
  the wave while emitting, a dark unlit version while disabled
- **Dark GUI (flyout)**: volume control, turn on/off, device selector, noise type
  and minimize/close buttons
- **Persistent configuration** in JSON (in the per-user config directory, see
  [Configuration](#configuration))
- **Audio backend**: miniaudio — WASAPI (Windows), CoreAudio (macOS), and
  PulseAudio/ALSA/JACK (Linux; PipeWire is reached through its PulseAudio or ALSA
  compatibility layer)
- **Adjustable noise level** (`0.002` by default ≈ -54 dBFS)
- **Selectable noise type**: white (default), pink, burst, speech band, low band or
  tone. White spreads its energy across the whole band, so a small BT speaker
  —which rolls off the low end— receives more useful signal to stay awake.
  **Burst** mode does not emit continuous noise: it fires a short noise pulse
  every few seconds, which is usually more effective (and less audible) when
  continuous noise does not stop the amplifier from sleeping. The three
  band-limited modes are probes for the case where the *speaker's own DSP* (not
  the amplifier) is the problem — it drops or clips the first syllable of each
  phrase because it does not see the signal it expects: **speech band** confines
  the noise to the voice range (300–3400 Hz), **low band** is a bass-only fill
  (45–130 Hz) that a small driver barely reproduces and **tone** is a steady sine
  at a frequency you pick, to sweep the spectrum and find what the speaker reacts
  to
- **Level auto-tune**: raises the level automatically if it detects drop-outs (the
  stream falls over or gets reconfigured), holds the raised level for a while and
  then eases it back down towards the level you set — never below it, so a fill
  that keeps a speaker from clipping words stays put. While it is on, the volume
  slider and the dB readout follow the level it settles on, and the level you set
  is shown as `(set … dBFS)` next to the checkbox; touching the slider, or using
  **Quieter -3 dB** / **Stronger +3 dB**, takes over from it
- **Volume remembered per noise type**: switching noise type restores the level
  that was tuned for it, so a level that works for one fill does not have to be
  re-found after trying another. Two buttons under the slider — **Quieter -3 dB**
  and **Stronger +3 dB** — nudge the level, which is the one thing the engine
  cannot work out on its own (it cannot hear whether the *speaker's* DSP is still
  clipping the first syllable of a word). The level is also capped per noise type
  so the fill can never reach full scale and distort
- **Output device selection** by index or default

## Requirements

- CMake >= 3.20
- Ninja (recommended) or MinGW Makefiles / Make
- MinGW-w64 (g++) on Windows, Xcode command line tools (clang) on macOS, GCC >= 9
  or clang on Linux (a C++17 `<filesystem>` is required)
- macOS: nothing beyond the Xcode command line tools; Cocoa, Carbon and OpenGL
  come from the system SDK
- Linux only:
  - the X11/Wayland and OpenGL headers the vendored GLFW is built against. On
    Debian/Ubuntu: `sudo apt install xorg-dev libwayland-dev libxkbcommon-dev
    libgl1-mesa-dev` (the `xorg-dev` metapackage pulls in every X11 header GLFW
    needs); on Fedora the `-devel` equivalents plus `pkgconf`. If Wayland is
    unavailable, configure with `-DGLFW_BUILD_WAYLAND=OFF`.
  - the D-Bus development headers for the tray (`libdbus-1-dev` on
    Debian/Ubuntu, `dbus-devel` on Fedora)
  - at least one audio backend's development headers (`libasound2-dev` for ALSA,
    `libpulse-dev` for PulseAudio or `libjack-dev` for JACK). CMake compiles only
    the backends it finds.
- Optional, to build the installers (see [Installers](#installers)):
  [Inno Setup 6](https://jrsoftware.org/isinfo.php) on Windows
  (`winget install JRSoftware.InnoSetup`); nothing extra on macOS/Linux

## Building

```bat
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The result is `build/white-sound.exe` (`white-sound` on macOS/Linux).

The flyout window is drawn with a portable **GLFW + OpenGL3** host (GLFW is
vendored in `third_party/glfw`, so there is nothing extra to install). The
original native **Win32 + DirectX 11** host is kept as a Windows-only fallback:

```bat
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release -DWS_FLYOUT_BACKEND=win32
```

The version is defined once, in the `project(white_sound VERSION x.y.z)` line at
the top of `CMakeLists.txt`. CMake expands `src/app/version.h.in` into
`ws_version.h` and `assets/white_sound.manifest.in` into the application
manifest, so the About dialog, the executable's file properties and the app
manifest all report the same version from that single place.

### Verifying on macOS/Linux

The macOS and Linux backends share the same portable shell as Windows but need a
first run on their own system. Configure and build as usual:

```sh
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Then launch the built binary and check, in order:

1. The **tray icon** appears (Linux needs a StatusNotifier host: KDE, or GNOME
   with the AppIndicator extension).
2. The **panel** opens on a left click, draws with rounded corners and closes on
   `Esc` or when it loses focus.
3. The **audio** starts (Turn on) and the header shows the opened device and
   sample rate; the log in the config directory records `opened ...`.
4. On macOS, **Ctrl+Option+W** toggles the panel and `About` reports
   `Global hotkey: active`.

Anything that fails is worth reporting with the exact tool output; the per-OS
code is small and lives behind `ws::platform::Tray` and
`ws::platform::showMessage()`.

## Platforms

The shell (`src/app/shell`) and everything below it are portable; only the tray
backend and the small platform services are per OS. CMake picks them by OS:

| OS | Tray backend | Notes |
| --- | --- | --- |
| Windows | `Shell_NotifyIcon` on a minimized anchor window (which is also the app's taskbar button) | also owns the global hotkey |
| macOS | `NSStatusItem` + `NSMenu` | global hotkey via Carbon |
| Linux | StatusNotifierItem + DBusMenu over the session D-Bus (libdbus) | needs a StatusNotifier host (KDE, or GNOME with an extension) |

The global toggle hotkey is **Ctrl+Alt+W** (**Ctrl+Option+W** on macOS). It is
implemented on Windows and macOS; the Linux tray backend does not register a
global hotkey yet, so use the tray icon there.

The single-instance guard and the message-box helper live in `src/app/platform`
and are per OS too (a named mutex on Windows, a `flock` lock file on
macOS/Linux).

> **Status:** the Windows backend is the one that has been built and run so far.
> The macOS and Linux backends drive the same portable shell but have not been
> built or run on those systems yet, so treat them as a starting point rather
> than a finished port. See
> [Verifying on macOS/Linux](#verifying-on-macoslinux) for the checks to run.

## Installers

Each platform has its own packaging script under `installers/` (see
[`installers/README.md`](installers/README.md)); build it on the target OS:

| Platform | Command | Output |
| --- | --- | --- |
| Windows | `powershell -ExecutionPolicy Bypass -File installers\windows\build_installer.ps1` | per-user Inno Setup installer (`installers\WhiteSoundSetup-<version>.exe`, no UAC) |
| macOS | `sh installers/macos/build_app.sh` | `installers/White Sound.app` + `White Sound-<version>.dmg` |
| Linux | `sh installers/linux/build_tarball.sh` | `installers/white-sound-<version>-linux-<arch>.tar.gz` (with `install.sh`) |

None of the packages are code-signed, so Windows SmartScreen and macOS Gatekeeper
warn on first run.

macOS packages can only be built on macOS (they need the Apple SDK). If you do not
have a Mac, the
[`build-installers` workflow](.github/workflows/build-installers.yml) builds all
three installers on GitHub's runners: run it manually (Actions → Build installers
→ Run workflow) to download them as artifacts, or push a `v*` tag to publish them
on a GitHub Release. See [`installers/README.md`](installers/README.md) for the
details.

## Usage

Run the application (`white-sound.exe` on Windows). It stays in the system
tray.

- **Left click** on the icon: opens/closes the **dark flyout** (compact, about
  320 px wide, with rounded corners)
  - Window buttons at the top right: **–** hides the panel (the app keeps
    emitting from the tray) and **✕** closes it. The first **✕** only steps back
    to the tray — the panel hides, the app keeps running and (on Windows) drops
    off the taskbar; a following **✕**, or **Exit** in the tray menu, exits for
    good and stops emitting
  - **Taskbar entry**: on Windows the app puts a taskbar button up when it is
    launched. **–** (and clicking away) just minimizes the panel the normal way,
    so the button stays and the panel can be restored from it; **✕** is what takes
    the button off the taskbar and leaves the app in the tray only. Once closed
    that way, opening the panel again from the tray (or the hotkey) does not bring
    the button back — it returns on the next launch
  - Status lamp (active/inactive) next to the label: the app icon drawn as a
    little bulb that lights up in blue with a soft halo while emitting, and stays
    as a dark unlit LED when disabled. It switches on as the panel opens and
    breathes gently while active. Right below is the **connected device** (the
    real endpoint the stream is using; when stopped, the selected one)
  - **Turn on/Turn off** button
  - **Device selector** (includes "Default")
  - **Volume slider** in dBFS (perceptual scale; adjusts the level live). The
    mouse wheel also works over the slider (±2 dB per notch). It warns when the
    level is so low that it may cause drop-outs, and when it has been limited
    because the active noise type would otherwise hit full scale. With
    **Auto-tune** on, the slider tracks the level the engine is actually using,
    so the dB readout moves when the auto-tuner adjusts it
  - **Quieter -3 dB** / **Stronger +3 dB**: one-click nudges of the level for the
    active noise type, the only feedback the engine cannot produce on its own
    (it cannot hear whether the speaker is still clipping words). The level is
    remembered per noise type
  - **Noise type**: White, Pink, Burst, Speech band, Low band or Tone. With
    Burst, two sliders appear for the pulse length and the interval between
    pulses; with Tone, one slider picks the frequency (20 Hz–8 kHz, logarithmic).
    Pointing at the box, or at an entry once the list is open, explains that one
    type only (the list itself stays free of explanations)
  - **Auto-tune**: raises the level on its own only when it detects drop-outs, then
    eases it back towards the level you set (never below it). The slider and the dB
    readout show the level in use; the level you set is shown next to the checkbox
  - **Brand mark**: the wave logo in its unlit (grey) state with *White Sound
    Project ©* sits small and dim in the bottom-right corner of the panel
  - **Moving the window**: drag from the top bar (the strip with the window
    buttons) or from the header. Only that bar lights up on hover (when you are
    not over a button), so the status text, the Turn off button and the device
    name keep their normal look. The window remembers where you left it and
    reopens there; to reopen it next to the cursor, **double click** the header
  - It closes on losing focus or with `Esc` (with the noise type list open, the
    first `Esc` closes the list and the next one closes the panel)
- **Right click** on the icon: context menu
  - **Open panel**: opens the flyout
  - **Enabled**: turns emission on/off
  - **Configuration...**: shows the current config and the file path
  - **About...**: version information
  - **Exit**: stops and quits
- **Global hotkey Ctrl+Alt+W** (Ctrl+Option+W on macOS): toggles the panel
  without reaching for the tray. On Linux the tray backend does not register a
  global hotkey yet.
- **Windows taskbar button**: while the app runs it also shows a single
  "White Sound" button on the taskbar, with its own icon. Clicking it toggles
  the panel (the panel still auto-hides when it loses focus). Closing the panel
  removes the button so the app lives only in the tray; reopening the panel from
  the tray brings it back

Changes made from the flyout are applied live and saved automatically.

## Configuration

The configuration file is JSON:

```json
{
  "deviceIndex": -1,
  "level": 0.005,
  "autoTune": true,
  "maxLevel": 0.06,
  "noiseMode": 0,
  "burstDurationMs": 200,
  "burstIntervalMs": 2000,
  "toneHz": 1000,
  "enabled": true,
  "levelsByMode": [0.005, 0, 0, 0, 0.031, 0]
}
```

- `deviceIndex`: `-1` = default device, or the device index
- `level`: noise amplitude (0.0 to 1.0; `0.002` ≈ -54 dBFS). This is the
  starting/base level the auto-tuner uses. It is the level of the **active**
  noise mode; the rest are kept in `levelsByMode`.
- `autoTune`: `true` = raises the level automatically on drop-outs (default)
- `maxLevel`: ceiling the auto-tuner may reach (`0.06` by default ≈ -24 dBFS).
  It is additionally clamped to the highest level the active noise type can play
  without hitting full scale
- `noiseMode`: `0` = white noise (default), `1` = pink noise, `2` = burst,
  `3` = speech band (300–3400 Hz), `4` = low band (bass, 45–130 Hz),
  `5` = tone (sine)
- `burstDurationMs`: length of each noise pulse in burst mode (`200` by default)
- `burstIntervalMs`: interval between pulses in burst mode (`2000` by default)
- `toneHz`: tone frequency in tone mode (`1000` by default, clamped to
  20–15000)
- `enabled`: `true` = start emitting at launch (default), `false` = start idle in
  the tray. Toggling **Turn on/Turn off** (or **Enabled** in the tray menu)
  updates it, so the app reopens in the state you left it
- `levelsByMode`: the level tuned for each noise mode, indexed like `noiseMode`
  (`0` = never tuned, in which case the current `level` is used). Switching noise
  type in the flyout saves the level of the mode being left and restores the one
  for the mode being entered

Default path, per OS:

- Windows: `%APPDATA%\white-sound\config.json`
- macOS: `~/Library/Application Support/white-sound/config.json`
- Linux: `$XDG_CONFIG_HOME/white-sound/config.json` (else
  `~/.config/white-sound/config.json`)

## Diagnostics

A log is written at startup and whenever the stream restarts, to
`ws.log` in the same per-user directory as the config (see
[Configuration](#configuration)): the device actually opened, sample rate, period
size, whether auto-routing is used, and every watchdog notification/restart. The
flyout shows the device actually opened in the header, below the status indicator
(and the sample rate below the selector).

With `autoTune` enabled, every detected drop-out is logged, e.g.
`autotune: 2 drop-out(s) -> level 0.002 -> 0.0045`, and the slow return is logged
at most every 30 s (`autotune: settling -> level …`). The volume slider and the
dB readout in the flyout show the level in use, and the level you set appears as
`(set … dBFS)` next to the checkbox whenever the tuner has moved it.

## Architecture

The code is layered so the audio engine, the application logic and the shell stay
portable, and only the small `ws::platform` layer touches the OS:

| Target | Platform | Contents |
| --- | --- | --- |
| `ws_core` | portable | audio engine (miniaudio) and the per-OS paths helper (config/log directory) |
| `imgui` | portable | Dear ImGui core only |
| `ws_gui` | portable | backend-agnostic flyout model + drawing, and the `ws::gui::Flyout` host interface |
| `ws_app` | portable | `ws::app::Controller`: configuration + engine orchestration |
| `ws_gui_glfw` | portable | GLFW + OpenGL3 flyout host (default) |
| `ws_gui_win32` | Windows | native Win32 + Direct3D 11 flyout host (fallback) |
| `white-sound` | per OS | entry point + shell (`src/app/shell`) + platform services and tray backend (`src/app/platform`) |

The shell talks to the window only through `ws::gui::Flyout` (show/hide, render,
events, focus) and to the tray only through `ws::platform::Tray` (icon, menu,
notifications, hotkey commands), so both are compile-time choices. The same
`ws::shell::run()` drives every OS; the entry points (`main_win32.cpp`,
`main_macos.cpp`, `main_linux.cpp`) only do the per-OS bootstrapping (DPI,
single instance) and then hand over. Everything platform-specific lives behind
`ws::platform::Tray` and `ws::platform::showMessage()`.

- `ws::KeepAlive` core, UI-independent (PIMPL, thread-safe level control)
- GUI with [Dear ImGui](https://github.com/ocornut/imgui): GLFW + OpenGL3 backend
  by default, native Win32 + DirectX 11 as a fallback (tray flyout)
- Audio backend: [miniaudio](https://github.com/mackron/miniaudio) (header-only):
  WASAPI on Windows, CoreAudio on macOS, PulseAudio/ALSA/JACK on Linux. CMake
  enables only the backend(s) it finds (`MA_ENABLE_ONLY_SPECIFIC_BACKENDS` +
  `MA_ENABLE_*`) and generates the name the About dialog reports
- Tray: `Shell_NotifyIcon` (Windows), `NSStatusItem` + `NSMenu` (macOS),
  StatusNotifierItem + DBusMenu over libdbus (Linux)
- Noise: white (uniform, xorshift32), pink (Paul Kellett approximation), burst
  (white noise pulses), speech band (band-passed noise, 300–3400 Hz), low band
  (band-passed noise, 45–130 Hz, the low window a speaker's DSP tends to gate on)
  or tone (sine), scaled to float32. The band-limited modes run through RBJ
  biquads (recomputed only when the mode or the sample rate changes) and get a
  measured makeup gain so the volume slider means about the same loudness in
  every mode; the applied gain is smoothed over ~20 ms so changing the volume or
  the mode does not click
- Config: [nlohmann/json](https://github.com/nlohmann/json) (header-only)
- App icons: two multi-size `.ico` files (lit and unlit) embedded through
  `assets/white_sound.rc`, so the tray icon, the window icons and the executable
  all share the same artwork. The macOS/Linux tray art is the same badge exported
  as PNG (`assets/white_sound.png`/`white_sound_off.png`) and the Linux tray uses
  the embedded ARGB pixmaps in `src/app/platform/tray_icons.h`
- Version and manifest: `project(... VERSION ...)` is the single source; CMake
  generates `ws_version.h` (used by the About dialog and the `VERSIONINFO`
  resource) and the application manifest (as-invoker, per-monitor-v2 DPI aware).
  The resource script embeds the manifest only for toolchains without one of
  their own (MSVC); MinGW-w64 links its default manifest and gets DPI awareness
  from the shell's own opt-in at startup (the GLFW host also opts in when it
  initialises)

## Notes

- Set the volume conservatively (`0.002` ≈ -54 dBFS) if you notice hiss or
  drop-outs.
- The level changes live without restarting the stream.

## Speaker clips the start of each word

Some Bluetooth speakers (small, cheap models with an aggressive internal DSP)
cut or mute the first syllable of every phrase during speech, podcasts or sports:
with only mid-band content and silence between words the speaker's DSP treats the
gap as "no signal" and shuts a gate or limiter, so the next word loses its attack.
This is a limitation of the speaker's firmware — the app cannot change it — but it
can feed the speaker the signal its DSP is looking for.

1. Keep the app emitting into the same output device the podcast uses.
2. **Turn off Auto-tune** so the level stops drifting back down.
3. Pick **Tone** and sweep the frequency between 40 Hz and 4 kHz while a podcast
   plays. The band where the clipping stops tells you what the speaker listens
   for.
4. Switch to the matching mode — **Low band** if the low end stopped it,
   **Speech band** if it was the voice range — and raise the level with the volume
   slider until the words are complete. **Turn Auto-tune** back on afterwards if
   you want it, but leave the level above the threshold that worked.

**Measured case:** on a ZEALOT S32 the clipping stopped only between 60 Hz and
100 Hz, so its gate is keyed on the low end. With **Low band** (45–130 Hz) the
speaker is fed exactly that window: the fill lands where the DSP is listening and,
because the driver is small, it is hardly audible. White or pink noise at a
listenable level never reaches enough bass energy to hold the gate open — the
white fill at -52 dBFS put roughly 38 dB less energy into 60–100 Hz than the tone
that fixed it. Set the slider around **-31 dBFS** in low band mode to match that
tone's energy; the mode only starts to clip above about -7 dBFS.

## License

MIT. See [LICENSE](LICENSE). Bundled third-party components (miniaudio, Dear
ImGui, GLFW and nlohmann/json) keep their own licenses; see
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
