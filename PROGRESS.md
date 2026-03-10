# Zelda Classic — macOS Port Progress

This is a running log of porting work. Append entries; do not rewrite history.
See `PORTING.md` for the full plan and `CLAUDE.md` for update instructions.

---

## 2026-03-10 — Planning complete

- Created `PORTING.md` with full 7-phase porting plan (Windows → macOS).
- Created `CLAUDE.md` with standing instructions to keep PORTING.md and PROGRESS.md current.
- Created `PROGRESS.md` (this file).

**Status**: No code changes yet. Planning phase complete.

**Phases**:
- [x] Phase 1 — Build System (CMake macOS branch)
- [~] Phase 2 — Allegro 4 → Allegro 5 migration (compat layer in place; per-file cleanup ongoing)
- [ ] Phase 3 — 32-bit → 64-bit code fixes
- [ ] Phase 4 — macOS platform wiring
- [ ] Phase 5 — Audio
- [ ] Phase 6 — App bundle & distribution
- [ ] Phase 7 — 32-bit quest file converter

---

## 2026-03-10 — Phase 1: Build System (CMake macOS branch)

**Status**: Complete.

**Files modified**:
- `CMakeLists.txt` — Phase 1 build system changes
- `src/zcmusic.h` — Phase 1 macOS visibility attributes

### Changes made

**`CMakeLists.txt`**:
1. Upgraded `CMAKE_CXX_STANDARD` from `98` to `14`. Required by Allegro 5 headers
   and eliminates C++98-isms caught by Clang under `-std=c++14`.
2. Added `elseif(APPLE)` platform branch (§3.1 of PORTING.md) between the MSVC and
   Linux blocks. Key contents:
   - Detects Allegro 5 via `pkg_check_modules` (works with Homebrew `brew install allegro`).
     Modules required: `allegro-5`, `allegro_font-5`, `allegro_ttf-5`, `allegro_audio-5`,
     `allegro_acodec-5`, `allegro_image-5`, `allegro_primitives-5`, `allegro_dialog-5`,
     `allegro_main-5`.
   - Links macOS system frameworks: Cocoa, OpenGL, AudioToolbox, CoreAudio, IOKit,
     CoreVideo, AppKit.
   - Defines `ALLEGRO_MACOSX` (existing `#ifdef` guards in source use this symbol).
   - No `-m32` flag — macOS build is 64-bit only (Phase 3 fixes the 32-bit assumptions).
3. Added `APPLE` branches to per-target sections:
   - `romview`: links `MACOS_FRAMEWORKS`.
   - `zelda`: includes `src/single_instance_unix.cpp` (Unix domain sockets work on macOS;
     §6.1 of PORTING.md), links `MACOS_FRAMEWORKS`.
   - `zquest`: same as zelda.
   - `zcsound`: defines `ZCM_DLL` / `ZCM_DLL_IMPORT` (same as Windows path, but symbols
     resolved via GCC visibility); sets `CXX_VISIBILITY_PRESET default`.

**`src/zcmusic.h`** (§3.3 of PORTING.md):
- Split the `ZCM_EXTERN` macro definition into three platform branches:
  - `_WIN32`: original `__declspec(dllexport/dllimport)` unchanged.
  - `__APPLE__`: `__attribute__((visibility("default")))` for both export and import.
  - All others: plain `extern`.

### Open items / next phase

- The source files still use Allegro 4 APIs — they will not compile against Allegro 5
  headers until Phase 2 (Allegro 4 → 5 migration) is complete.
- The old Allegro 4 static libs in `libs/osx/` are not yet removed; they will be
  deleted at the start of Phase 2 (§3.2 of PORTING.md).
- Known risk: `#pragma pack` and MSVC vs Clang struct layout differences remain open
  (addressed in Phase 3/7).

---

## 2026-03-10 — Phase 2: Allegro 4 → Allegro 5 migration (compat layer)

**Status**: Compat layer created; source files not yet individually migrated.

**Files created**:
- `src/allegro5_compat.h` — Allegro 4 API surface implemented on Allegro 5
- `src/allegro5_compat.cpp` — Implementation of the compat layer

**Files modified**:
- `src/zc_alleg.h` — Switches to compat header on `__APPLE__`; preserves Allegro 4
  path on Windows/Linux
- `CMakeLists.txt` — Adds `allegro5_compat.cpp` to all four targets (zcsound, romview,
  zelda, zquest) on APPLE

**Files deleted** (stale Allegro 4 macOS artifacts):
- `allegro/src/macosx/*.m` (15 Objective-C files: cadigi, camidi, drivers, hidjoy,
  hidman, keybd, main, pcpu, qtmidi, quartz, qzfull, qzmouse, qzwindow, soundman,
  system)
- `libs/osx/*.a` (12 static libraries: aldmb, algif, almp3, alogg, alspc, dumb,
  gme, jpgal, ldpng, png, stdc++-static, z)

### What the compat layer provides

**`allegro5_compat.h`** (~600 lines) covers:
- Fixed-point `fixed` type + `itofix`/`fixtoi`/`ftofix`/`fixtof` + full `fix` C++
  class with all operators (replaces `allegro/fix.h`)
- `RGB` / `PALETTE` types + `set_palette`, `get_palette`, `set_color`, `get_color`
- `makecol(r,g,b)` → nearest palette index search; `getr/getg/getb` palette accessors
- `BITMAP` struct with `w`, `h`, `clip`, `cl`/`cr`/`ct`/`cb`, and `line[]` row-pointer
  array backed by an internal `uint8_t*` 8-bit pixel buffer.  Provides the same
  `bmp->line[y][x]` access that Allegro 4 code uses throughout.
- All drawing primitives: `blit`, `masked_blit`, `stretch_blit`, `draw_sprite` and
  flipped variants, `putpixel`/`getpixel`, `rectfill`, `rect`, `line`, `hline`,
  `vline`, `circle`, `circlefill`, `ellipse`, `ellipsefill`, `triangle`, `floodfill`
- `FONT` struct + stub vtable for `char_length`/`text_length`/`font_height`
- `textout_ex`, `textout_centre_ex`, `textout_right_ex`, `textprintf_ex` and friends
  (render via Allegro 5 font → temp RGBA bitmap → palette-approximate back to 8-bit)
- Full `KEY_*` → `ALLEGRO_KEY_*` constant mapping; `volatile char key[ALLEGRO_KEY_MAX]`
  array updated from Allegro 5 events; `key_shifts`, `readkey`, `ureadkey`, `keypressed`
- Mouse: `volatile int mouse_x`, `mouse_y`, `mouse_b`; updated from Allegro 5 events
- `set_gfx_mode` → `al_create_display`; `allegro_init` → `al_init` + addon inits
- `install_timer` / `install_int` / `install_int_ex` → software timer callbacks fired
  from `_al5_pump_events()` each frame
- `PACKFILE` → thin `ALLEGRO_FILE*` wrapper; all pack_fread/pack_fwrite/pack_getc/
  pack_igetl etc. implemented
- `DIALOG` / `MENU` structs and all `D_*` / `MSG_*` constants (jwin.cpp compatibility)
- `set_config_file` / `get_config_string` etc. → Allegro 5 config API
- `GFX_*` mode constants, `DRAW_MODE_*`, `COLOR_MAP` type, sound stubs, CPU globals

**`allegro5_compat.cpp`** (~700 lines) implements:
- Palette management: `_al5_bestfit_color` (nearest-palette search), `_al5_color`,
  `_al5_rebuild_color_cache`
- `BITMAP` constructor/destructor; `upload_to_al5()` (converts 8-bit data → RGBA
  ALLEGRO_BITMAP via locked region, for final display)
- All drawing primitives (pure C++ software renderer on the 8-bit pixel buffer)
- `_al5_pump_events()`: drains Allegro 5 event queue → updates `key[]`, `mouse_*`,
  fires software timer callbacks
- `_al5_flip_display()`: calls `screen->upload_to_al5()`, draws to backbuffer,
  calls `al_flip_display()`
- Text rendering via Allegro 5 font → temp `ALLEGRO_MEMORY_BITMAP` → palette scan
- PACKFILE I/O including big-endian and little-endian multi-byte helpers
- Config via `ALLEGRO_CONFIG`; dialog stubs

### Known open items (Phase 2 in-progress)

The compat layer gives macOS builds a complete Allegro 4 API, but some source files
use deep Allegro 4 internals that need per-file manual migration:

1. **`bmp->line[y][x]` for non-8-bit operations** — `ending.cpp`, `jwin.cpp`,
   `zq_class.cpp`, `zc_sys.cpp` access `->line[]` treating pixels as 8-bit palette
   indices (already correct with our `uint8_t*` buffer).  But some uses in `jwin.cpp`
   cast `line[0]` to `short*` — these need per-site migration.

2. **`FONT` vtable access** — `zq_strings.cpp` calls
   `workfont->vtable->char_length(workfont, c)` directly.  The stub vtable in
   `allegro5_compat.h` makes this compile, but real glyph widths require each font
   to have a proper vtable.  Fix: ensure `FONT` objects constructed by `load_font`
   have their vtable `char_length` wired to the Allegro 5 font.

3. **Dialog/GUI layer** — `do_dialog`, `popup_dialog`, `init_dialog` are stubbed as
   no-ops.  The `jwin.cpp` GUI system needs a full Allegro 5 event-driven reimplementation
   (targeted for Phase 4).

4. **`zqscale.cpp` vtable proxy** — `zqwin_set_clip` patches a vtable pointer directly;
   needs a macOS-specific code path.

5. **`create_sub_bitmap`** — currently creates an independent copy rather than a
   view into the parent.  Any writes to the parent after sub-bitmap creation won't
   appear in the sub.  Flag for Phase 4 fix.

6. **Rotation / gouraud primitives** — stubbed as copies/masked blits.  Rotation
   effects in-game will look wrong until a software scanline rasteriser is added.

7. **`allegro/src/macosx/` directory** — now empty after deleting the 15 Allegro 4
   ObjC files; directory itself can be removed at a future cleanup pass.

### Decision log

- Chose **8-bit internal pixel buffer** (not 32-bit ALLEGRO_BITMAP internally) for
  `BITMAP` because the game's existing `->line[y][x]` direct-pixel accesses, the
  palette cycle effects, and the `blit`/`masked_blit` performance all rely on 1
  byte per pixel.  Only the final `screen` flip converts to 32-bit RGBA.
- Chose **software timer callbacks** (polling from `_al5_pump_events`) rather than
  Allegro 5 `ALLEGRO_TIMER` event sources, because the game installs multiple
  callbacks with `install_int_ex` at different rates and the polling model is simpler
  to retrofit without changing the game loop structure.
- Chose **nearest-palette search** for `makecol` to match Allegro 4 8-bit behaviour
  exactly.  This is O(256) per call but only called during setup, not per-frame.
