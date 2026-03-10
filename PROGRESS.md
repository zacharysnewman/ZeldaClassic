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
- [ ] Phase 2 — Allegro 4 → Allegro 5 migration
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
