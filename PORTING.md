# Zelda Classic — macOS Porting Plan

This document is the canonical reference for the Windows → macOS port of Zelda Classic.
It covers every phase of the work, known risks, and the rationale behind key decisions.

> **Living document**: If you discover a new challenge, a wrong assumption, or complete a
> phase, update this file. See `CLAUDE.md` for the standing instruction.

---

## Table of Contents

1. [Context & Goals](#1-context--goals)
2. [Decision: Native Port via Allegro 5](#2-decision-native-port-via-allegro-5)
3. [Phase 1 — Build System (CMake macOS Branch)](#3-phase-1--build-system-cmake-macos-branch)
4. [Phase 2 — Allegro 4 → Allegro 5 Migration](#4-phase-2--allegro-4--allegro-5-migration)
5. [Phase 3 — 32-bit → 64-bit Code Fixes](#5-phase-3--32-bit--64-bit-code-fixes)
6. [Phase 4 — macOS Platform Wiring](#6-phase-4--macos-platform-wiring)
7. [Phase 5 — Audio](#7-phase-5--audio)
8. [Phase 6 — App Bundle & Distribution](#8-phase-6--app-bundle--distribution)
9. [Phase 7 — 32-bit Quest File Converter](#9-phase-7--32-bit-quest-file-converter)
10. [Known Risks & Open Questions](#10-known-risks--open-questions)
11. [Reference: Affected Files](#11-reference-affected-files)

---

## 1. Context & Goals

Zelda Classic is a ~231,000-line C++ game engine built on **Allegro 4.2.2** (a heavily
modified fork). It targets three executables: `zelda` (player), `zquest` (editor), and
`romview` (tile ripper), plus a shared `zcsound` library.

The codebase was originally Windows-only (MSVC 2008). A Linux port was added later and
is functional. A macOS port was started but abandoned — infrastructure artifacts remain
(Objective-C Allegro files, `/libs/osx/` prebuilt libs, `.plist` files, `#ifdef
ALLEGRO_MACOSX` guards) but **there is no working macOS CMake build target**.

### Goals

- Build and run all three executables natively on macOS (Apple Silicon and Intel).
- Maintain full compatibility with existing Windows and Linux builds.
- Preserve backwards compatibility with existing `.qst` quest files (with a converter
  for older 32-bit binary files — see Phase 7).
- Do not rewrite game logic; this is a platform port, not a remake.

### Non-Goals

- A game engine rewrite (Unity, Godot, SDL-from-scratch, etc.).
- Breaking changes to the ZScript API.
- Dropping Windows or Linux support.

---

## 2. Decision: Native Port via Allegro 5

### Why not a game engine rewrite?

The codebase is 231K lines of C++ with a custom scripting engine, a quest editor, a
tile ripper, and a decades-old community of quest authors. Rewriting in Unity or Godot
would mean rewriting all game logic, the scripting VM, and the editor — not just the
platform layer. That is a separate multi-year project, not a port.

### Why Allegro 5 instead of patching Allegro 4?

| Concern | Allegro 4 (current) | Allegro 5 |
|---|---|---|
| macOS support | Incomplete / abandoned | First-class, actively maintained |
| 64-bit | Not designed for it | Native |
| Apple Silicon | No | Yes (via universal binary) |
| OpenGL / Metal backend | Quartz only | OpenGL / Metal |
| Audio | OSS / ALSA only | Multiple backends incl. CoreAudio |
| Maintenance | Dead upstream | Active |

The primary cost of Allegro 5 is **API breakage**: Allegro 5 is not backwards-compatible
with Allegro 4. The migration is significant but well-understood (documented below in
Phase 2). It is still far less work than a game engine rewrite.

### Why not SDL2?

SDL2 is another viable option, but Allegro 5 is closer to Allegro 4 conceptually
(same coordinate system, similar bitmap/palette model, similar input model), reducing
migration surface. SDL2 would require mapping more foreign abstractions.

---

## 3. Phase 1 — Build System (CMake macOS Branch)

**Goal**: `cmake .. && make` produces macOS binaries (even if they crash immediately).
Everything else depends on having a working build.

### 3.1 Add macOS branch to `CMakeLists.txt`

The current `CMakeLists.txt` (254 lines) has branches for MSVC (Windows) and GCC
(Linux) but nothing for macOS (Clang/AppleClang). Add a third branch:

```cmake
if(MSVC)
    # existing Windows config
elseif(APPLE)
    # new macOS config
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++14 -Wall")
    set(PLATFORM_LIBS "-framework Cocoa -framework OpenGL -framework AudioToolbox
                       -framework CoreAudio -framework IOKit")
    add_definitions(-DALLEGRO_MACOSX)
    # link against Allegro 5 (homebrew or bundled)
else()
    # existing Linux config
endif()
```

Drop the `-m32` flag for macOS (64-bit only; see Phase 3).

### 3.2 Replace `/libs/osx/` with Allegro 5

The existing `/libs/osx/` contains prebuilt Allegro 4 static libraries. These are
stale and incompatible. Replace with Allegro 5 obtained via:

- **Development**: Homebrew (`brew install allegro`) — easiest for iteration.
- **Distribution**: Bundle Allegro 5 as a static library compiled from source, included
  in the repo or fetched via CMake `FetchContent`. This avoids a runtime Homebrew
  dependency for end users.

Use CMake `find_package(Allegro5 REQUIRED)` with a fallback to bundled static libs.

### 3.3 `zcsound` shared library

`zcsound` is compiled as a `.dll` on Windows. On macOS it becomes a `.dylib`. The
`ZCM_DLL` / `ZCM_DLL_IMPORT` defines are MSVC-specific. Add macOS equivalents:

```cpp
#if defined(__APPLE__)
  #define ZCM_EXPORT __attribute__((visibility("default")))
  #define ZCM_IMPORT __attribute__((visibility("default")))
#elif defined(_WIN32)
  #define ZCM_EXPORT __declspec(dllexport)
  #define ZCM_IMPORT __declspec(dllimport)
#endif
```

### 3.4 Compiler / standard upgrade

The codebase targets C++98. The Allegro 5 headers require at least C++11. Upgrade the
CMake CXX standard to **C++14** (conservative; avoids touching game logic). Audit for
any C++98-isms that become errors under C++14 (common ones: `register` keyword, old
`auto`, implicit `int` return).

### Deliverable

A macOS CMake build that compiles without linker errors (runtime failures acceptable
at this stage).

---

## 4. Phase 2 — Allegro 4 → Allegro 5 Migration

This is the largest single phase. Allegro 5's API is a clean redesign; there is no
automatic migration. Work through each subsystem in order.

### 4.1 Display / Window Management

| Allegro 4 | Allegro 5 equivalent |
|---|---|
| `set_gfx_mode(GFX_AUTODETECT_WINDOWED, w, h, 0, 0)` | `al_create_display(w, h)` |
| `GFX_QUARTZ_FULLSCREEN` | `al_set_new_display_flags(ALLEGRO_FULLSCREEN)` |
| `GFX_QUARTZ_WINDOW` | `al_set_new_display_flags(ALLEGRO_WINDOWED)` |
| `set_color_depth(8)` | Palette emulation — see §4.3 |
| `screen` global bitmap | `al_get_backbuffer(display)` |

Key file: `src/zc_sys.cpp` (13+ macOS-guarded sections).

### 4.2 Bitmap / Drawing

| Allegro 4 | Allegro 5 equivalent |
|---|---|
| `BITMAP*` | `ALLEGRO_BITMAP*` |
| `create_bitmap(w, h)` | `al_create_bitmap(w, h)` |
| `blit(src, dst, ...)` | `al_draw_bitmap_region(...)` |
| `draw_sprite(...)` | `al_draw_bitmap(...)` with transforms |
| `putpixel(bmp, x, y, c)` | `al_put_pixel(x, y, color)` |
| `rectfill(bmp, ...)` | `al_draw_filled_rectangle(...)` |

Allegro 5 targets are set via `al_set_target_bitmap()`, replacing the implicit
"current bitmap" model of Allegro 4.

### 4.3 Palette / 8-bit Color

Zelda Classic uses an **8-bit indexed palette** throughout (game colors, tile palettes,
cycle effects). Allegro 5 dropped native 8-bit mode.

**Migration strategy**: Render to an `ALLEGRO_BITMAP` in 32-bit RGBA, but maintain the
game's internal 8-bit index arrays and palette tables unchanged. At render time, expand
index → RGBA using the current palette. This keeps all game logic untouched while
outputting full-color pixels to the display.

Implement a `PaletteRenderer` class that:
1. Takes the game's existing 256-color palette (`RGB pal[256]`).
2. Converts 8-bit index bitmaps to RGBA `ALLEGRO_BITMAP` on each frame.
3. Wraps `al_map_rgb()` for color lookups.

This is additional work but fully isolates the palette system from the display system.

### 4.4 Input

| Allegro 4 | Allegro 5 equivalent |
|---|---|
| `poll_keyboard()` / `key[]` array | `al_get_keyboard_state()` / `ALLEGRO_KEYBOARD_STATE` |
| `poll_joystick()` | `al_get_joystick_state()` |
| `poll_mouse()` / `mouse_x`, `mouse_y` | `al_get_mouse_state()` |
| `KEY_*` constants | `ALLEGRO_KEY_*` constants |

Allegro 5 also introduces an **event queue** model. Input is read from
`ALLEGRO_EVENT_QUEUE` rather than polled globals. The game loop will need to drain the
event queue each frame.

### 4.5 Font / Text Rendering

| Allegro 4 | Allegro 5 equivalent |
|---|---|
| `FONT*` | `ALLEGRO_FONT*` |
| `textout_ex(bmp, font, str, x, y, fg, bg)` | `al_draw_text(font, color, x, y, flags, str)` |
| `allegro_404_char` | `al_set_fallback_font()` |

Allegro 5 font rendering uses the `allegro_font` and `allegro_ttf` addons.

### 4.6 Timer

| Allegro 4 | Allegro 5 equivalent |
|---|---|
| `install_timer()` / `LOCK_VARIABLE` / `LOCK_FUNCTION` | `al_create_timer(rate)` + event queue |
| `install_int_ex(func, rate)` | `al_register_event_source(queue, al_get_timer_event_source(timer))` |

The game loop timer model changes significantly: interrupt-based callbacks become
event-driven. The main loop in `zelda.cpp` and `zquest.cpp` will need restructuring.

### 4.7 File I/O & Path Utilities

Allegro 4's `PACKFILE` API (`pack_fopen`, `pack_fclose`, `pack_getc`) is replaced by
Allegro 5's `ALLEGRO_FILE` API (`al_fopen`, `al_fclose`, `al_fgetc`). The quest file
loader in `qst.cpp` (14,656 lines) uses `PACKFILE` / `pfread` / `pfwrite` extensively
— this is a large mechanical substitution.

Allegro 5 also provides `al_get_standard_path()` for resolving platform-correct app
data, documents, and temp directories, replacing the manual `macosx_qst_dir` path
logic in `zelda.cpp` and `zquest.cpp`.

### 4.8 Miscellaneous Allegro 4 APIs to Remove

- `allegro_init()` → `al_init()`
- `install_allegro(SYSTEM_NONE, ...)` → remove (no equivalent needed)
- `set_window_title(str)` → `al_set_window_title(display, str)`
- `desktop_color_depth()` → `al_get_display_option(display, ALLEGRO_COLOR_SIZE)`
- `get_desktop_resolution(&w, &h)` → `al_get_monitor_info(0, &info)`
- `vsync()` → `al_flip_display()` (vsync controlled by display flags)
- `show_mouse(bmp)` / `hide_mouse()` → `al_show_mouse_cursor(display)` / `al_hide_mouse_cursor(display)`

### Deliverable

All three executables launch and display the main menu on macOS without crashing.

---

## 5. Phase 3 — 32-bit → 64-bit Code Fixes

**Do not start this phase until Phase 2 is complete.** The Allegro 5 migration will
itself eliminate some 32-bit assumptions; fixing them in parallel creates noise.

The audit identified **263 issues across 17 key files**. Work through them in this
order (highest blast radius first):

### 5.1 Replace `long` with `int32_t` in data structures

The `mapscr` struct in `zdefs.h` uses `long` for FFC positions and script variables.
On Linux/macOS 64-bit, `long` is 8 bytes; on Windows 32-bit it is 4 bytes. This breaks
binary serialization.

**Fix**: Replace every `long` used as a game-data type with `int32_t` (or `int64_t`
where 64-bit range is actually needed). Use a project-wide find/replace, then manually
audit each change.

Priority struct members (zdefs.h lines 1362–1376):
```cpp
// Before
long ffx[32], ffy[32];
long ffxdelta[32], ffydelta[32];
long initd[32][8];

// After
int32_t ffx[32], ffy[32];
int32_t ffxdelta[32], ffydelta[32];
int32_t initd[32][8];
```

Total: 92 `long` declarations to audit across the codebase.

### 5.2 Fix pointer-to-integer casts

28 instances of `(int)pointer` across `zelda.cpp`, `zquest.cpp`, `ffscript.cpp`,
`guys.cpp`, `weapons.cpp`. These silently truncate 64-bit addresses.

**Fix**:
- Debug/logging uses: replace `(int)ptr` with `(uintptr_t)ptr` and `%x` with `%p`.
- `(int)this` in logger name generation: replace with a monotonically incrementing
  static counter so logger names are unique without using an address.
- `(int)parent->x` / `(int)posx` casts in physics: these are likely casting `float`
  or `fixed` to `int` for integer arithmetic, not casting pointers — audit each one to
  confirm the type and use an explicit `static_cast<int>()` to make intent clear.

### 5.3 Fix `sizeof(long)` in `memset`/`memcpy` calls

12 instances in `ffscript.cpp`, `zelda.cpp`, `maps.cpp`:
```cpp
// Before
memset(ffc_stack[i], 0, 256 * sizeof(long));

// After (after step 5.1 changes the type)
memset(ffc_stack[i], 0, 256 * sizeof(int32_t));
```

These are mechanical fixes that follow naturally from step 5.1.

### 5.4 Fix `strlen()` / `sizeof()` → `int` truncations

24 instances in `jwin.cpp`, `zq_strings.cpp`, `ending.cpp`, `zdefs.h`. These will not
crash but produce compiler warnings on 64-bit that can mask real errors.

**Fix**: Use `static_cast<int>(strlen(...))` with a comment, or better, retype the
local variables as `size_t` and adjust the surrounding logic.

### 5.5 Fix Windows HANDLE cast

Two instances in `ffscript.cpp:499` and `zquest.cpp:595`:
```cpp
// Before
long lStdHandle = (long)GetStdHandle(STD_OUTPUT_HANDLE);

// After
intptr_t lStdHandle = (intptr_t)GetStdHandle(STD_OUTPUT_HANDLE);
```

### 5.6 C++98 → C++14 cleanup

With `-std=c++14`, the compiler will flag deprecated constructs used in the codebase:
- Remove `register` keyword from variable declarations.
- Replace `NULL` with `nullptr` where type safety matters.
- Replace C-style casts with `static_cast` / `reinterpret_cast` where identified above.

### Deliverable

All three executables build and run on macOS without 64-bit truncation warnings.
The test suite (if added) passes on both 32-bit Linux and 64-bit macOS.

---

## 6. Phase 4 — macOS Platform Wiring

These are macOS-specific integration points not covered by the Allegro 5 migration.

### 6.1 Single-instance check

Linux uses `single_instance_unix.cpp` (Unix domain sockets). macOS can reuse this
file — Unix domain sockets work on macOS. Verify and add to the macOS CMake source
list. If issues arise, a macOS-native alternative is `NSRunningApplication`.

### 6.2 File system paths

Replace the manual `macosx_qst_dir` / `macosx_data_path` string building with
Allegro 5's `al_get_standard_path()`:

| Path | Allegro 5 constant |
|---|---|
| Application bundle resources | `ALLEGRO_RESOURCES_PATH` |
| User documents (quests) | `ALLEGRO_USER_DOCUMENTS_PATH` |
| User app data (saves) | `ALLEGRO_USER_DATA_PATH` |
| Temp files | `ALLEGRO_TEMP_PATH` |

### 6.3 HiDPI / Retina

Allegro 5 supports HiDPI via `ALLEGRO_DISPLAY_OPTION_SCALE_RENDERING`. The game
renders at fixed 320×240 and scales up — ensure the scaling path works correctly on
Retina displays (2× and 3× backing scales). Test with `ALLEGRO_GENERATE_EXPOSE_EVENTS`
disabled to avoid double-render artifacts.

### 6.4 Metal / OpenGL backend

macOS deprecated OpenGL in 10.14. Allegro 5.2.9+ supports the Metal backend via
MoltenVK. Enable it when targeting macOS 11+:
```cmake
if(APPLE)
    find_library(METAL_FRAMEWORK Metal)
    if(METAL_FRAMEWORK)
        target_link_libraries(zelda ${METAL_FRAMEWORK})
        add_definitions(-DALLEGRO_METAL)
    endif()
endif()
```

### Deliverable

Game launches, loads a quest, and plays correctly on macOS including correct file paths
and HiDPI scaling.

---

## 7. Phase 5 — Audio

Audio is a separate risk area. The current stack is:
- `almp3` — MP3 playback
- `alogg` — Ogg Vorbis
- `dumb` — MOD/IT/XM/S3M tracker music
- `gme` — NSF, SPC, VGM chip music
- `aldmb` — MIDI

Allegro 5 includes the `allegro_audio` and `allegro_acodec` addons which handle
most of these via platform-native backends (CoreAudio on macOS).

### 5.1 Replace Allegro 4 sound addons with Allegro 5 equivalents

| Allegro 4 addon | Allegro 5 path |
|---|---|
| `almp3` | `allegro_acodec` (built-in MP3 via dr_mp3 or system decoder) |
| `alogg` | `allegro_acodec` (built-in Ogg via libvorbis) |
| `dumb` | Link `libdumb` directly; Allegro 5 has a DUMB addon (`allegro_audio` + custom loader) |
| `gme` | Link `libgme` directly; write a custom `ALLEGRO_AUDIO_STREAM` source |
| `aldmb` / MIDI | CoreMIDI on macOS; use `al_open_native_dialog` or a soft-synth like TiMidity |

### 5.2 `zcsound` library rewrite

`zcmusic.cpp` and `zcmusicd.cpp` implement `zcsound`. Rewrite the internal audio
backend against Allegro 5's audio API while keeping the public `zcmusic_*` function
signatures unchanged (they are called from game code). This minimizes changes to the
callers.

### Deliverable

Music and SFX play correctly in-game on macOS. MIDI either plays via CoreMIDI or
falls back gracefully to silence with a log warning.

---

## 8. Phase 6 — App Bundle & Distribution

### 8.1 `.app` bundle structure

```
ZeldaClassic.app/
  Contents/
    Info.plist
    MacOS/
      zelda          (main executable)
    Frameworks/
      liballegro5.dylib   (or static link)
      libzcsound.dylib
    Resources/
      zelda.dat
      sfx.dat
      fonts/
      ...
```

The existing `info1.plist` and `info2.plist` in the repo are starting points.
Update with correct bundle IDs, minimum macOS version (11.0 recommended), and
`NSHighResolutionCapable = YES`.

### 8.2 CMake install rules

Add CMake `install()` rules and a `MACOSX_BUNDLE` target property to produce the
`.app` automatically:
```cmake
set_target_properties(zelda PROPERTIES
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_SOURCE_DIR}/resources/Info.plist
)
```

### 8.3 Code signing & notarization

For distribution outside the Mac App Store:
1. Sign with an Apple Developer ID certificate: `codesign --deep --sign "Developer ID Application: ..."`.
2. Notarize with `xcrun notarytool submit`.
3. Staple: `xcrun stapler staple`.

For development/open-source builds, ad-hoc signing suffices:
`codesign --deep --sign - ZeldaClassic.app`

### 8.4 GitHub Actions CI

Add a `.github/workflows/macos.yml` that:
1. Installs Allegro 5 via Homebrew.
2. Configures with CMake (`-DCMAKE_BUILD_TYPE=Release -DAPPLE=ON`).
3. Builds all targets.
4. Runs smoke tests (launch and exit cleanly).
5. (Optional) Produces a notarized `.dmg` artifact on tagged releases.

### Deliverable

A `.dmg` containing `ZeldaClassic.app` that launches on a clean macOS 11+ system
without Homebrew or Xcode installed.

---

## 9. Phase 7 — 32-bit Quest File Converter

**This is the final phase.** It is deliberately last because:
- The correct serialized format on 64-bit is only known after Phases 2–3 stabilize it.
- The converter must read the old format, which requires understanding both formats.
- Existing quest authors must not be left with unloadable files.

### 9.1 Problem statement

The `.qst` quest file format serializes raw C++ structs to disk using `pfwrite()`.
Because the `mapscr` struct contains `long` arrays (4 bytes on 32-bit Windows, 8 bytes
on 64-bit), existing `.qst` files written on 32-bit Windows have a different binary
layout than the 64-bit macOS build expects.

Additionally, struct alignment/padding may differ between MSVC (Windows) and
Clang (macOS), even at the same bit-width.

### 9.2 New versioned format

Before writing the converter, stabilize the on-disk format:

1. Add a **format version field** to the `.qst` header (the header already has a
   magic number and version; increment the version for the 64-bit format).
2. Replace raw `pfread(buf, sizeof(mapscr), f)` calls with field-by-field explicit
   serialization using fixed-width types (`int32_t`, `uint8_t`, etc.).
3. Document the new format schema in `docs/qst-format.md`.

This makes the format architecture-independent and enables the converter.

### 9.3 Converter implementation

Create a standalone tool `tools/qst_convert/main.cpp` that:

1. Opens a `.qst` file.
2. Reads the version field from the header.
3. If version < NEW_FORMAT_VERSION:
   a. Reads each struct using the **old 32-bit layout** (hard-coded field offsets
      based on the Windows 32-bit MSVC struct layout).
   b. Deserializes into in-memory C++ structs using `int32_t` fields.
   c. Writes a new `.qst` file using the new versioned field-by-field serializer.
4. If version == NEW_FORMAT_VERSION: copies the file unchanged (idempotent).
5. Reports any unrecognized chunks as warnings (older custom data).

The converter should be a **separate CMake target** (`qst_convert`) that can be run
standalone without launching the full game.

```cmake
add_executable(qst_convert tools/qst_convert/main.cpp)
target_link_libraries(qst_convert ${ALLEGRO5_LIBRARIES})
```

### 9.4 Backwards compatibility in the loader

The quest loader in `qst.cpp` should also be updated to detect old-format files and
offer a conversion prompt (or auto-convert with a backup):

```
Found quest file in legacy 32-bit format.
Converting to current format... (backup saved as quest.qst.bak)
```

### 9.5 Community migration

- Release the converter as a standalone download (does not require installing the full game).
- Provide a drag-and-drop wrapper (Automator workflow or shell script) for non-technical users.
- Announce the format change in release notes with a clear migration guide.

### Deliverable

- `qst_convert` tool builds and converts all known test `.qst` files correctly.
- The game loader auto-detects and converts on first open with a backup.
- A sample of community quest files verified to load correctly on macOS.

---

## 10. Known Risks & Open Questions

| Risk | Severity | Status | Notes |
|---|---|---|---|
| Allegro 5 palette emulation performance | Medium | Open | Full-screen 8→32-bit expansion per frame; benchmark needed |
| MIDI on macOS without a soft synth | Medium | Open | CoreMIDI requires a soundfont; may need to bundle one |
| Allegro 5 API coverage gaps | Medium | Open | Some Allegro 4 behavior may have no direct A5 equivalent |
| `#pragma pack` in serialized structs | High | Open | Audit needed; MSVC and Clang may handle differently |
| 32-bit save files (game saves, not quests) | High | Open | Same `long`-in-struct issue applies to `.sav` files; needs same treatment |
| ZScript VM memory layout | High | Open | Script stack uses `sizeof(long)` — must verify scripts still execute correctly after Phase 3 |
| Objective-C Allegro 4 files in `allegro/src/macosx/` | Low | Resolved | Will be replaced by Allegro 5; delete these files in Phase 2 |
| Apple Silicon (arm64) | Low | Open | Allegro 5 supports arm64; universal binary (`lipo`) may be needed for Intel compatibility |
| Game save file format | High | Open | `.sav` files have the same `long`-in-struct problem as `.qst` files; a save converter may also be needed alongside the quest converter |

---

## 11. Reference: Affected Files

### Critical (must change)

| File | Lines | Why |
|---|---|---|
| `CMakeLists.txt` | 254 | Add macOS branch |
| `src/zdefs.h` | 2929 | 92 `long` declarations; core data structures |
| `src/qst.cpp` | 14,656 | 50+ raw struct reads/writes; format versioning |
| `src/ffscript.cpp` | 11,384 | Script runtime; `sizeof(long)` stacks |
| `src/zquest.cpp` | 28,951 | Main editor; platform wiring |
| `src/zelda.cpp` | 4,761 | Game core; platform wiring |
| `src/zc_sys.cpp` | ~3,000 | 13+ `#ifdef ALLEGRO_MACOSX` sections |
| `src/zcmusic.cpp` | — | Audio backend rewrite |
| `src/zcmusicd.cpp` | — | Audio backend rewrite |

### High (significant changes)

| File | Why |
|---|---|
| `src/guys.cpp` | 15+ `(int)parent->x` position casts |
| `src/weapons.cpp` | 10+ `(int)posx` casts |
| `src/zq_tiles.cpp` | 6+ raw struct file I/O |
| `src/maps.cpp` | `sizeof(long)` memsets |
| `src/jwin.cpp` | 24 `strlen()→int` truncations |
| `src/zq_strings.cpp` | strlen/size_t casting |
| `src/link.cpp` | Platform-specific path logic |

### Low (minor changes or delete)

| File | Why |
|---|---|
| `allegro/src/macosx/*.m` | Delete; replaced by Allegro 5 |
| `libs/osx/*.a` | Delete; replaced by Allegro 5 |
| `info1.plist`, `info2.plist` | Update for modern macOS; move to `resources/` |
| `src/win32.cpp` | Already `#ifdef _WIN32` guarded; no change needed |
| `src/single_instance_unix.cpp` | Reuse on macOS; add to macOS CMake target |
