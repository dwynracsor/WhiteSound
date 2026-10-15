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
- **Selectable noise type**: white (default), pink or burst. White spreads its
  energy across the whole band, so a small BT speaker —which rolls off the low
  end— receives more useful signal to stay awake. **Burst** mode does not emit
  continuous noise: it fires a short noise pulse every few seconds, which is
  usually more effective (and less audible) when continuous noise does not stop
  the amplifier from sleeping
- **Level auto-tune**: if it detects drop-outs (the stream falls over or gets
  reconfigured), it raises the level automatically up to a ceiling and brings it
  down slowly while stable, so it never gets dirtier than needed. The level never
  goes below the **learned floor** (the one needed to survive the last drop-out),
  so it does not fall back into the "sleep → raise → lower" cycle
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
- [Inno Setup 6](https://jrsoftware.org/isinfo.php) — only to build the Windows
  installer (`winget install JRSoftware.InnoSetup`)

## Building

```bat
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The result is `build/white_sound_tray.exe` (`white_sound_tray` on macOS/Linux).

The flyout window is drawn with a portable **GLFW + OpenGL3** host (GLFW is
vendored in `third_party/glfw`, so there is nothing extra to install). The
original native **Win32 + DirectX 11** host is kept as a Windows-only fallback:

```bat
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release -DWS_FLYOUT_BACKEND=win32
```

### Unit tests

The pure parts of the audio engine (the auto-tuner step and the burst generator)
are covered by a small, dependency-free test executable. It is off by default:

```bat
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release -DWS_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

The version is defined once, in the `project(white_sound VERSION x.y.z)` line at
the top of `CMakeLists.txt`. CMake expands `src/app/version.h.in` into
`ws_version.h` and `assets/white_sound.manifest.in` into the application
manifest, so the About dialog, the executable's file properties and the app
manifest all report the same version from that single place.

### Verifying on macOS/Linux

The Windows build is exercised on every change; the macOS and Linux backends
share the same portable shell but still need a first run on their own system.
`tools/verify_unix.sh` does the configure + build + unit-test pass in one step:

```sh
sh tools/verify_unix.sh
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

The packaging scripts ([Packaging](#packaging)) give something concrete to
install and launch on those systems: `packaging/linux/build_tarball.sh` builds a
tarball whose `install.sh` places the binary, desktop entry and icons, and
`packaging/macos/build_app.sh` builds `White Sound.app` and a `.dmg`. Installing
the package and repeating the checks above is the real end-to-end test.

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
> [Verifying on macOS/Linux](#verifying-on-macoslinux) for the one-command
> check.

## Packaging

Each OS gets a self-contained package; none of them are code-signed, so Windows
SmartScreen and macOS Gatekeeper will warn on first run.

### Windows (Inno Setup)

```powershell
powershell -ExecutionPolicy Bypass -File installer\build_installer.ps1
```

Builds the application and packages it with Inno Setup into
`dist\WhiteSoundSetup-<version>.exe`.

The install is per-user and needs no admin rights (no UAC prompt): it copies the
executable (plus `README.md`, `LICENSE` and `THIRD_PARTY_NOTICES.md`) to
`%LOCALAPPDATA%\Programs\White Sound`, adds a Start Menu shortcut
and — optionally — a desktop shortcut and a
`HKCU\...\CurrentVersion\Run` entry to start with Windows. It also ships an
uninstaller that removes those shortcuts and the autostart entry. Your settings
in `%APPDATA%\white-sound` are left untouched (delete that folder to reset).

The installer reads its version from the build, so it always matches the app.

To check the installer end to end, there is a script that installs silently,
verifies the files, shortcuts, registry entries and version, launches the
installed app and confirms its tray window appears, then uninstalls and verifies
that everything is gone:

```powershell
powershell -ExecutionPolicy Bypass -File installer\test_install.ps1
```

It is safe to run on a development machine (it removes a previous install
first), but it is meant for a clean Windows machine or VM. It exits non-zero if
any check fails.

### macOS (`.app` + `.dmg`)

```sh
sh packaging/macos/build_app.sh
```

Builds the app and assembles `dist/White Sound.app` (executable in
`Contents/MacOS`, the tray PNGs and a generated `.icns` in `Contents/Resources`,
plus an `Info.plist` with `LSUIElement` so it is a tray-only agent) and a
`dist/White Sound-<version>.dmg` with an `/Applications` shortcut for
drag-to-install. The bundle is only ad-hoc signed, so the first launch needs a
**right-click → Open**.

### Linux (portable tarball)

```sh
sh packaging/linux/build_tarball.sh
```

Builds the app and produces `dist/white-sound-<version>-linux-<arch>.tar.gz`
containing `bin/`, `share/` (a `.desktop` entry and the hicolor theme icons) and
an `install.sh`. To install:

```sh
tar xzf white-sound-<version>-linux-<arch>.tar.gz
sh white-sound-<version>-linux-<arch>/install.sh          # into ~/.local
sh white-sound-<version>-linux-<arch>/install.sh --system # into /usr/local
```

`install.sh` also takes `--autostart` (writes an `~/.config/autostart` entry) and
`--uninstall`. Installing the icon into the theme is what lets the desktop's tray
host resolve the `IconName` the StatusNotifierItem advertises.

## Usage

Run the application (`white_sound_tray.exe` on Windows). It stays in the system
tray.

- **Left click** on the icon: opens/closes the **dark flyout** (compact, about
  320 px wide, with rounded corners)
  - Window buttons at the top right: **–** hides the panel (the app keeps
    emitting from the tray) and **✕** closes it. The first **✕** only steps back
    to the tray — the panel hides, the app keeps running and (on Windows) drops
    off the taskbar; a following **✕**, or **Exit** in the tray menu, exits for
    good and stops emitting
  - Status lamp (active/inactive) next to the label: the app icon drawn as a
    little bulb that lights up in blue with a soft halo while emitting, and stays
    as a dark unlit LED when disabled. It switches on as the panel opens and
    breathes gently while active. Right below is the **connected device** (the
    real endpoint the stream is using; when stopped, the selected one)
  - **Turn on/Turn off** button
  - **Device selector** (includes "Default")
  - **Volume slider** in dBFS (perceptual scale; adjusts the level live). The
    mouse wheel also works over the slider (±2 dB per notch). It warns when the
    level is so low that it may cause drop-outs
  - **Noise type**: White, Pink or Burst. With Burst, two sliders appear for the
    pulse length and the interval between pulses
  - **Auto-tune**: raises the level only when it detects drop-outs; shows the
    effective level once it has adjusted it
  - **Moving the window**: drag from the top bar (the strip with the window
    buttons) or from the header. Only that bar lights up on hover (when you are
    not over a button), so the status text, the Turn off button and the device
    name keep their normal look. The window remembers where you left it and
    reopens there; to reopen it next to the cursor, **double click** the header
  - It closes on losing focus or with `Esc`
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
  "maxLevel": 0.02,
  "noiseMode": 0,
  "burstDurationMs": 200,
  "burstIntervalMs": 2000,
  "enabled": true
}
```

- `deviceIndex`: `-1` = default device, or the device index
- `level`: noise amplitude (0.0 to 1.0; `0.002` ≈ -54 dBFS). This is the
  starting/base level the auto-tuner uses.
- `autoTune`: `true` = raises the level automatically on drop-outs (default)
- `maxLevel`: ceiling the auto-tuner may reach (`0.02` by default ≈ -34 dBFS)
- `noiseMode`: `0` = white noise (default), `1` = pink noise, `2` = burst
- `burstDurationMs`: length of each noise pulse in burst mode (`200` by default)
- `burstIntervalMs`: interval between pulses in burst mode (`2000` by default)
- `enabled`: `true` = start emitting at launch (default), `false` = start idle in
  the tray. Toggling **Turn on/Turn off** (or **Enabled** in the tray menu)
  updates it, so the app reopens in the state you left it

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

With `autoTune` enabled, every detected drop-out and every level adjustment is
logged, e.g. `autotune: 2 drop-out(s) -> level 0.002 -> 0.0045`. The flyout shows
the effective level (`Auto: -47 dBFS`) when the tuner has raised it above the
base level.

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
| `white_sound_tray` | per OS | entry point + shell (`src/app/shell`) + platform services and tray backend (`src/app/platform`) |

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
- Noise: white (uniform, xorshift32), pink (Paul Kellett approximation) or burst
  (white noise pulses), scaled to float32
- Config: [nlohmann/json](https://github.com/nlohmann/json) (header-only)
- App icons: two multi-size `.ico` files (lit and unlit) generated by
  `tools/make_icon.ps1` and embedded through `assets/white_sound.rc`, so the tray
  icon, the window icons and the executable all share the same artwork. The
  macOS/Linux tray art is the same badge exported as PNG
  (`assets/white_sound.png`/`white_sound_off.png`); `tools/make_tray_icons.ps1`
  turns those into the embedded ARGB pixmaps used by the Linux tray
  (`src/app/platform/tray_icons.h`)
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

## License

MIT. See [LICENSE](LICENSE). Bundled third-party components (miniaudio, Dear
ImGui, GLFW and nlohmann/json) keep their own licenses; see
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
