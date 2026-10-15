cmake_minimum_required(VERSION 3.20)
project(white_sound VERSION 1.0.0 LANGUAGES CXX)

# Objective-C++ is required for the macOS tray (NSStatusItem) and the macOS
# platform services, which are .mm sources. Without this, no enabled language
# claims the .mm extension, so CMake quietly leaves those files out of the build
# and createTray()/acquireSingleInstance()/showMessage() end up undefined at link
# time. Only macOS uses Objective-C++.
if(APPLE)
    enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Single source of truth for the version. src/app/version.h.in is expanded with
# PROJECT_VERSION into ws_version.h, which the C++ code (About dialog) and the
# Windows resource script (VERSIONINFO) both include. The application manifest is
# generated the same way so its assembly version stays in sync. Bump the version
# in the project() call above only.
set(WS_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${WS_GENERATED_DIR})
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/version.h.in
    ${WS_GENERATED_DIR}/ws_version.h
    @ONLY
)
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/white_sound.manifest.in
    ${WS_GENERATED_DIR}/white_sound.manifest
    @ONLY
)
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/installer/version.iss.in
    ${WS_GENERATED_DIR}/version.iss
    @ONLY
)

# Core library: the portable audio engine plus the per-OS paths helper. Has no
# dependency on the window/tray layer, so it can be reused on every platform.
add_library(ws_core
    src/core/keepalive.h
    src/core/audio.cpp
    src/core/paths.h
    src/core/paths.cpp
)

target_include_directories(ws_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Platform-specific miniaudio configuration.
#
# miniaudio compiles every backend whose platform headers it can see, so on
# Linux it needs the -dev packages of all of them. MA_ENABLE_ONLY_SPECIFIC_BACKENDS
# plus the matching MA_ENABLE_* compile exactly the backend(s) we found, so only
# the found backend's headers are required.
#
# NOTE: MA_API is miniaudio's linkage/visibility attribute (it expands to `extern`
# or a visibility attribute), NOT a backend selector. Redefining it to a backend
# token breaks every miniaudio declaration ("'MA_API_...' does not name a type")
# and silently makes getBackendName() report the wrong backend.
set(WS_AUDIO_BACKEND_NAME "Unknown")

if(WIN32)
    target_compile_definitions(ws_core PRIVATE MA_WIN32_LEAN_AND_MEAN)
    target_link_libraries(ws_core PRIVATE ole32 oleaut32)
    # Left on miniaudio's defaults (WASAPI first, then DirectSound/WinMM).
    set(WS_AUDIO_BACKEND_NAME "WASAPI")
elseif(APPLE)
    find_library(COREAUDIO CoreAudio)
    find_library(AUDIO_TOOLBOX AudioToolbox)
    find_library(AUDIO_UNIT AudioUnit)
    target_link_libraries(ws_core PRIVATE ${COREAUDIO} ${AUDIO_TOOLBOX} ${AUDIO_UNIT})
    target_compile_definitions(ws_core PRIVATE
        MA_ENABLE_ONLY_SPECIFIC_BACKENDS
        MA_ENABLE_COREAUDIO
    )
    set(WS_AUDIO_BACKEND_NAME "CoreAudio")
elseif(UNIX AND NOT APPLE)
    # Linux/BSD. miniaudio has no native PipeWire backend: on a PipeWire system it
    # goes through the pipewire-alsa or pipewire-pulse compatibility layer, so we
    # look for those instead. Enable every backend we find and let miniaudio pick
    # at runtime; the name shown to the user is the preferred one.
    find_package(PkgConfig QUIET)
    set(WS_MINI_BACKENDS MA_ENABLE_ONLY_SPECIFIC_BACKENDS)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(PULSE libpulse QUIET)
        pkg_check_modules(ALSA alsa QUIET)
        pkg_check_modules(JACK jack QUIET)
    endif()

    if(PULSE_FOUND)
        list(APPEND WS_MINI_BACKENDS MA_ENABLE_PULSEAUDIO)
        target_link_libraries(ws_core PRIVATE ${PULSE_LIBRARIES})
        target_include_directories(ws_core PRIVATE ${PULSE_INCLUDE_DIRS})
    endif()
    if(ALSA_FOUND)
        list(APPEND WS_MINI_BACKENDS MA_ENABLE_ALSA)
        target_link_libraries(ws_core PRIVATE ${ALSA_LIBRARIES})
        target_include_directories(ws_core PRIVATE ${ALSA_INCLUDE_DIRS})
    endif()
    if(JACK_FOUND)
        list(APPEND WS_MINI_BACKENDS MA_ENABLE_JACK)
        target_link_libraries(ws_core PRIVATE ${JACK_LIBRARIES})
        target_include_directories(ws_core PRIVATE ${JACK_INCLUDE_DIRS})
    endif()

    if(PULSE_FOUND)
        set(WS_AUDIO_BACKEND_NAME "PulseAudio")
    elseif(ALSA_FOUND)
        set(WS_AUDIO_BACKEND_NAME "ALSA")
    elseif(JACK_FOUND)
        set(WS_AUDIO_BACKEND_NAME "JACK")
    else()
        list(APPEND WS_MINI_BACKENDS MA_ENABLE_NULL)
        set(WS_AUDIO_BACKEND_NAME "Null (silent)")
        message(WARNING "No Linux audio backend found (install libpulse-dev or "
                        "libasound2-dev); miniaudio will use its null backend")
    endif()
    target_compile_definitions(ws_core PRIVATE ${WS_MINI_BACKENDS})
endif()

# The backend name shown in the About dialog. Generated like the version header
# so it stays in sync with the selection above instead of guessing at runtime.
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/audio_backend.h.in
    ${WS_GENERATED_DIR}/ws_audio_backend.h
    @ONLY
)
target_include_directories(ws_core PRIVATE ${WS_GENERATED_DIR})

# Dear ImGui, core library only. Rendering backends are added per platform
# (the Windows host pulls in the Win32 + DirectX11 backends below).
add_library(imgui STATIC
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
)
target_include_directories(imgui PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui
)
target_compile_definitions(imgui PRIVATE IMGUI_DISABLE_OBSOLETE_FUNCTIONS)

# Portable flyout UI: the backend-agnostic model and its ImGui drawing code. No
# OS or rendering-backend dependency.
add_library(ws_gui STATIC
    src/app/gui/gui_ui.cpp
)
target_include_directories(ws_gui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(ws_gui PRIVATE imgui ws_core)

# Portable application logic: owns the audio engine and the persisted config and
# translates GUI/state changes into engine calls. Knows nothing about the tray,
# the window or the message loop, so the same core can back any OS shell.
add_library(ws_app STATIC
    src/app/app_controller.cpp
)
target_include_directories(ws_app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_include_directories(ws_app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_link_libraries(ws_app PRIVATE ws_core ws_gui)

# Window backend for the flyout. The portable GLFW + OpenGL3 host is the default
# on every platform; the native Win32 + Direct3D 11 host is kept as a fallback
# (Windows only). Exactly one of them provides ws::gui::createFlyout().
set(WS_FLYOUT_BACKEND "glfw" CACHE STRING "Flyout window backend (glfw or win32)")
set_property(CACHE WS_FLYOUT_BACKEND PROPERTY STRINGS glfw win32)

if(WS_FLYOUT_BACKEND STREQUAL "glfw")
    # Vendored GLFW, built for the host platform (Win32 on Windows, Cocoa on
    # macOS, X11/Wayland on Linux).
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
    add_subdirectory(third_party/glfw EXCLUDE_FROM_ALL)

    find_package(OpenGL REQUIRED)

    add_library(ws_gui_glfw STATIC
        src/app/gui/gui_glfw.cpp
        third_party/imgui/backends/imgui_impl_glfw.cpp
        third_party/imgui/backends/imgui_impl_opengl3.cpp
    )
    target_include_directories(ws_gui_glfw PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(ws_gui_glfw PUBLIC imgui ws_gui glfw OpenGL::GL)
elseif(WS_FLYOUT_BACKEND STREQUAL "win32")
    if(NOT WIN32)
        message(FATAL_ERROR "WS_FLYOUT_BACKEND=win32 is only available on Windows")
    endif()
    # Native flyout host: Win32 + Direct3D 11 ImGui backends.
    add_library(ws_gui_win32 STATIC
        src/app/gui/gui_win32_dx11.cpp
        third_party/imgui/backends/imgui_impl_win32.cpp
        third_party/imgui/backends/imgui_impl_dx11.cpp
    )
    target_include_directories(ws_gui_win32 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(ws_gui_win32
        PUBLIC imgui ws_gui
        PRIVATE ws_core d3d11 dxgi d3dcompiler dwmapi
    )
else()
    message(FATAL_ERROR "Unknown WS_FLYOUT_BACKEND '${WS_FLYOUT_BACKEND}' (use glfw or win32)")
endif()

# System tray application. The shell (src/app/shell) is portable: it drives the
# controller and the flyout through interfaces only. The tray backend and the
# platform services it links are chosen per OS:
#   Windows  Shell_NotifyIcon on a hidden window (which owns the global hotkey)
#   macOS    NSStatusItem + NSMenu (+ a Carbon global hotkey)
#   Linux    StatusNotifierItem + DBusMenu over the session D-Bus (libdbus)
set(WS_TRAY_SOURCES
    src/app/shell/shell.cpp
)
set(WS_PLATFORM_SOURCES
    src/app/platform/tray.h
    src/app/platform/platform.h
)

if(WIN32)
    list(APPEND WS_TRAY_SOURCES src/app/shell/main_win32.cpp)
    list(APPEND WS_PLATFORM_SOURCES
        src/app/platform/platform_win32.cpp
        src/app/platform/tray_win32.cpp
    )
elseif(APPLE)
    list(APPEND WS_TRAY_SOURCES src/app/shell/main_macos.cpp)
    list(APPEND WS_PLATFORM_SOURCES
        src/app/platform/platform_macos.mm
        src/app/platform/tray_macos.mm
    )
else()
    list(APPEND WS_TRAY_SOURCES src/app/shell/main_linux.cpp)
    list(APPEND WS_PLATFORM_SOURCES
        src/app/platform/platform_linux.cpp
        src/app/platform/tray_linux.cpp
        src/app/platform/tray_icons.h
    )
endif()

add_executable(white_sound_tray ${WS_TRAY_SOURCES} ${WS_PLATFORM_SOURCES})
target_include_directories(white_sound_tray PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    # Generated ws_version.h (and, on Windows, the manifest).
    ${WS_GENERATED_DIR}
)
target_link_libraries(white_sound_tray PRIVATE ws_app ws_gui)
if(WS_FLYOUT_BACKEND STREQUAL "win32")
    target_link_libraries(white_sound_tray PRIVATE ws_gui_win32)
else()
    target_link_libraries(white_sound_tray PRIVATE ws_gui_glfw)
endif()

if(WIN32)
    # Compile assets/white_sound.rc so the .exe carries the app icon and Windows
    # can show it in Explorer, the tray and the file properties dialog.
    enable_language(RC)
    target_sources(white_sound_tray PRIVATE assets/white_sound.rc)
    target_include_directories(white_sound_tray PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
        # windres resolves white_sound.ico through the include path.
        ${CMAKE_CURRENT_SOURCE_DIR}/assets
    )
    target_link_libraries(white_sound_tray PRIVATE shell32 user32 comctl32 gdi32)
    target_compile_definitions(white_sound_tray PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
    if(MINGW)
        set_target_properties(white_sound_tray PROPERTIES LINK_FLAGS "-static -mwindows")
    else()
        set_target_properties(white_sound_tray PROPERTIES WIN32_EXECUTABLE TRUE)
        target_compile_definitions(white_sound_tray PRIVATE WS_EMBED_MANIFEST)
    endif()
elseif(APPLE)
    find_library(COCOA_FRAMEWORK Cocoa REQUIRED)
    find_library(CARBON_FRAMEWORK Carbon REQUIRED)
    target_link_libraries(white_sound_tray PRIVATE
        ${COCOA_FRAMEWORK} ${CARBON_FRAMEWORK})
else()
    # Linux tray: StatusNotifierItem over D-Bus, via libdbus.
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(DBUS dbus-1)
    endif()
    if(DBUS_FOUND)
        target_include_directories(white_sound_tray PRIVATE ${DBUS_INCLUDE_DIRS})
        target_link_libraries(white_sound_tray PRIVATE ${DBUS_LIBRARIES})
    else()
        # No pkg-config (or no dbus-1.pc): locate the header and the library
        # ourselves, so dbus/dbus.h is on the include path either way.
        find_path(DBUS_INCLUDE_DIR NAMES dbus/dbus.h REQUIRED)
        find_library(DBUS_LIBRARY NAMES dbus-1 REQUIRED)
        target_include_directories(white_sound_tray PRIVATE ${DBUS_INCLUDE_DIR})
        target_link_libraries(white_sound_tray PRIVATE ${DBUS_LIBRARY})
    endif()
endif()

# Optional unit tests for the portable, pure helpers (auto-tuner and burst
# generator). Off by default so a normal build does not compile the audio engine
# twice; enable with -DWS_BUILD_TESTS=ON and run `ctest --test-dir build`.
option(WS_BUILD_TESTS "Build the unit tests" OFF)
if(WS_BUILD_TESTS)
    enable_testing()
    add_executable(ws_tests tests/test_keepalive.cpp)
    target_include_directories(ws_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        # Generated ws_audio_backend.h.
        ${WS_GENERATED_DIR}
    )
    target_link_libraries(ws_tests PRIVATE ws_core)
    add_test(NAME ws_tests COMMAND ws_tests)
endif()
