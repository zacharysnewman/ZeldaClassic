// allegro5_compat.h — Allegro 4 → Allegro 5 compatibility layer
//
// Used on macOS (__APPLE__) builds only.  Provides the Allegro 4 API surface
// the existing Zelda Classic codebase depends on, implemented on top of
// Allegro 5 with a software 8-bit palette renderer.
//
// Design notes (see PORTING.md §4.3):
//   • All BITMAPs carry an internal uint8_t* data buffer (8-bit palette indices).
//   • The `line[y]` row-pointer array gives the same direct pixel access that
//     Allegro 4 code expects (e.g. bmp->line[y][x] = color_index).
//   • At frame display time, the `screen` bitmap's internal buffer is converted
//     palette→RGBA and uploaded to an ALLEGRO_BITMAP for hardware display.
//   • Colors passed to drawing primitives are palette indices (0–255) or packed
//     RGB values returned by makecol().  The helper _al5_color() maps either to
//     the current ALLEGRO_COLOR for display uploads.
//
// Phase 2 limitations (to be resolved in later iterations):
//   • FONT vtable access (zq_strings.cpp, jwin.cpp) is stubbed; those call-sites
//     will need per-file manual migration (Phase 2 TODO).
//   • 3-D / rotate / gouraud primitives are stub no-ops.
//   • The dialog/GUI system (DIALOG, do_dialog, popup_dialog) is declared but
//     not implemented; jwin.cpp will need an Allegro-5-native GUI layer.
//   • Audio (SAMPLE, MIDI) is forwarded to zcsound / Phase 5.

#pragma once
#ifndef _ALLEGRO5_COMPAT_H_
#define _ALLEGRO5_COMPAT_H_

// ============================================================
// Allegro 5 includes
// ============================================================
#include <allegro5/allegro.h>
#include <allegro5/allegro_font.h>
#include <allegro5/allegro_ttf.h>
#include <allegro5/allegro_image.h>
#include <allegro5/allegro_audio.h>
#include <allegro5/allegro_acodec.h>
#include <allegro5/allegro_primitives.h>
#include <allegro5/allegro_native_dialog.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/time.h>
#include <cmath>
#include <vector>
#include <algorithm>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================
// Misc compatibility macros
// ============================================================
#ifndef INLINE
#define INLINE static inline
#endif

// Allegro 4 const / attribute helpers
#define AL_CONST        const
#define AL_VAR(t,n)     extern t n
#define AL_ARRAY(t,n)   extern t n[]
#define AL_FUNC(r,n,a)  r n a
#define AL_METHOD(r,n,a) r (*n) a
#define AL_FUNCPTR(r,n,a) extern r (*n) a

#ifndef MIN
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a,b) ((a) > (b) ? (a) : (b))
#endif
#ifndef ABS
#define ABS(a) ((a) < 0 ? -(a) : (a))
#endif
#ifndef SGN
#define SGN(a) ((a) < 0 ? -1 : ((a) > 0 ? 1 : 0))
#endif
#ifndef MID
#define MID(a,b,c) MAX((a), MIN((b), (c)))
#endif
#ifndef CLAMP
#define CLAMP(a,b,c) MID(a,b,c)
#endif

#ifndef TRUE
#define TRUE  1
#define FALSE 0
#endif

#define AL_ASSERT(c)  ((void)(c))
#define ASSERT(c)     ((void)(c))
#define END_OF_MAIN() /* no-op */

// ============================================================
// Fixed-point arithmetic  (replaces allegro/fixed.h)
// ============================================================
typedef int32_t fixed;

#define itofix(x)   ((fixed)((x) << 16))
#define fixtoi(x)   ((int)((unsigned)(x) >> 16))
#define ftofix(x)   ((fixed)((x) * 65536.0))
#define fixtof(x)   ((float)(x) / 65536.0)

#define fixadd(x,y)  ((x) + (y))
#define fixsub(x,y)  ((x) - (y))
static inline fixed fixmul(fixed x, fixed y) { return (fixed)(((int64_t)x * y) >> 16); }
static inline fixed fixdiv(fixed x, fixed y) { return (fixed)(((int64_t)x << 16) / y); }

// Fixed-point math functions (implemented in allegro5_compat.cpp)
fixed fixsin(fixed x);
fixed fixcos(fixed x);
fixed fixtan(fixed x);
fixed fixasin(fixed x);
fixed fixacos(fixed x);
fixed fixatan(fixed x);
fixed fixatan2(fixed y, fixed x);
fixed fixsqrt(fixed x);
fixed fixhypot(fixed a, fixed b);

// ============================================================
// Palette types
// ============================================================
typedef struct _RGB { unsigned char r, g, b; } RGB;
typedef RGB PALETTE[256];

extern PALETTE _zc_palette;       // current active palette
extern bool    _zc_palette_dirty; // rebuild color cache on next use

// Map 8-bit palette index to ALLEGRO_COLOR (for display upload)
ALLEGRO_COLOR _al5_color(int idx);

// Find nearest palette entry for an RGB triple (used by makecol)
int _al5_bestfit_color(int r, int g, int b);

// Rebuild the internal palette→ALLEGRO_COLOR cache
void _al5_rebuild_color_cache();

// Palette API
void set_palette(const PALETTE p);
void get_palette(PALETTE p);
void set_color(int idx, const RGB* rgb);
void get_color(int idx, RGB* rgb);

// Color component extraction from a palette index
static inline int getr(int idx) {
    if (idx >= 0 && idx < 256) return _zc_palette[idx].r;
    return (idx >> 16) & 0xFF;
}
static inline int getg(int idx) {
    if (idx >= 0 && idx < 256) return _zc_palette[idx].g;
    return (idx >> 8) & 0xFF;
}
static inline int getb(int idx) {
    if (idx >= 0 && idx < 256) return _zc_palette[idx].b;
    return idx & 0xFF;
}
static inline int getr8(int idx) { return getr(idx); }
static inline int getg8(int idx) { return getg(idx); }
static inline int getb8(int idx) { return getb(idx); }

// makecol: find nearest palette index for (r,g,b).  In 8-bit mode Allegro 4
// does the same search.  makecol32 etc. store direct RGB in the high bytes.
#define makecol(r,g,b)   _al5_bestfit_color(r, g, b)
#define makecol8(r,g,b)  _al5_bestfit_color(r, g, b)
// Direct RGB packing for non-palette contexts (transparent layers etc.)
static inline int makecol32(int r, int g, int b) {
    return (r << 16) | (g << 8) | b;
}

// ============================================================
// BITMAP struct — 8-bit software renderer
// ============================================================
// All pixel data is stored as uint8_t palette indices.  The `line[]` array of
// row pointers mirrors Allegro 4's bmp->line[y][x] access pattern.
// An ALLEGRO_BITMAP* (_al5bmp) is attached only to `screen` (and video bitmaps
// that are displayed directly); it is uploaded from the 8-bit buffer each frame.
struct BITMAP {
    int w, h;
    int clip;           // clipping enabled flag
    int cl, cr, ct, cb; // clip rectangle (left, right, top, bottom)

    // Row pointers: line[y] points to the start of row y in _data.
    // Always valid (never null for a constructed BITMAP).
    uint8_t** line;

    // Internal flat 8-bit pixel buffer (w*h bytes, row-major).
    uint8_t* _data;

    // Allegro 5 hardware bitmap for display; null for pure in-memory bitmaps.
    ALLEGRO_BITMAP* _al5bmp;
    bool _owns_al5bmp; // whether we should al_destroy_bitmap on destruction

    // Initialise a new w×h bitmap (data zeroed)
    BITMAP(int _w, int _h);
    ~BITMAP();

    // Upload 8-bit data → _al5bmp using the current palette.
    // Creates _al5bmp on first call if it is null.
    void upload_to_al5();

private:
    // Rebuild line[] to point into _data.  Called after (re)allocation.
    void _rebuild_line_ptrs();
};

// ============================================================
// Global screen pointer + display dimensions
// ============================================================
extern BITMAP* screen;

extern int _zc_screen_w;
extern int _zc_screen_h;

#define SCREEN_W  (_zc_screen_w)
#define SCREEN_H  (_zc_screen_h)

// Allegro 5 display / event objects
extern ALLEGRO_DISPLAY*     _al5_display;
extern ALLEGRO_EVENT_QUEUE* _al5_event_queue;
extern ALLEGRO_TIMER*       _al5_timer;

// ============================================================
// Bitmap construction / destruction
// ============================================================
BITMAP* create_bitmap(int w, int h);
BITMAP* create_bitmap_ex(int bpp, int w, int h);
BITMAP* create_video_bitmap(int w, int h);
BITMAP* create_system_bitmap(int w, int h);
BITMAP* create_sub_bitmap(BITMAP* parent, int x, int y, int w, int h);
void    destroy_bitmap(BITMAP* bmp);

// Locking — no-op in the software renderer (line[] is always accessible)
static inline void acquire_bitmap(BITMAP* bmp)  { (void)bmp; }
static inline void release_bitmap(BITMAP* bmp)  { (void)bmp; }
static inline void acquire_screen()             {}
static inline void release_screen()             {}

// Bitmap property queries
static inline int bitmap_color_depth(BITMAP* bmp) { (void)bmp; return 8; }
static inline int bitmap_mask_color(BITMAP* bmp)  { (void)bmp; return 0; }
static inline int is_memory_bitmap(BITMAP* bmp)   { return bmp &&  bmp->_al5bmp == nullptr; }
static inline int is_video_bitmap(BITMAP* bmp)    { return bmp && (bmp->_al5bmp != nullptr); }
static inline int is_screen_bitmap(BITMAP* bmp)   { return bmp && bmp == screen; }

// ============================================================
// Pixel operations
// ============================================================
static inline void putpixel(BITMAP* bmp, int x, int y, int c)
{
    if (!bmp) return;
    if (bmp->clip) {
        if (x < bmp->cl || x >= bmp->cr || y < bmp->ct || y >= bmp->cb) return;
    }
    if (x < 0 || x >= bmp->w || y < 0 || y >= bmp->h) return;
    bmp->line[y][x] = (uint8_t)c;
}

static inline int getpixel(BITMAP* bmp, int x, int y)
{
    if (!bmp || x < 0 || x >= bmp->w || y < 0 || y >= bmp->h) return -1;
    return bmp->line[y][x];
}

// Aliases used in some files
static inline void _putpixel(BITMAP* bmp, int x, int y, int c) { putpixel(bmp, x, y, c); }
static inline int  _getpixel(BITMAP* bmp, int x, int y)        { return getpixel(bmp, x, y); }

// ============================================================
// Drawing primitives — all operate on the 8-bit data buffer
// ============================================================
void clear_bitmap(BITMAP* bmp);
void clear_to_color(BITMAP* bmp, int color);

void blit(BITMAP* src, BITMAP* dst, int sx, int sy, int dx, int dy, int w, int h);
void masked_blit(BITMAP* src, BITMAP* dst, int sx, int sy, int dx, int dy, int w, int h);
void stretch_blit(BITMAP* src, BITMAP* dst, int sx, int sy, int sw, int sh,
                  int dx, int dy, int dw, int dh);
void masked_stretch_blit(BITMAP* src, BITMAP* dst, int sx, int sy, int sw, int sh,
                         int dx, int dy, int dw, int dh);

void draw_sprite(BITMAP* dst, BITMAP* sprite, int x, int y);
void draw_sprite_v_flip(BITMAP* dst, BITMAP* sprite, int x, int y);
void draw_sprite_h_flip(BITMAP* dst, BITMAP* sprite, int x, int y);
void draw_sprite_vh_flip(BITMAP* dst, BITMAP* sprite, int x, int y);
void draw_trans_sprite(BITMAP* dst, BITMAP* sprite, int x, int y);
void draw_lit_sprite(BITMAP* dst, BITMAP* sprite, int x, int y, int color);
void stretch_sprite(BITMAP* dst, BITMAP* sprite, int x, int y, int w, int h);
void masked_stretch_sprite(BITMAP* dst, BITMAP* sprite, int x, int y, int w, int h);
void rotate_sprite(BITMAP* dst, BITMAP* sprite, int x, int y, fixed angle);
void rotate_scaled_sprite(BITMAP* dst, BITMAP* sprite, int x, int y, fixed angle, fixed scale);
void pivot_sprite(BITMAP* dst, BITMAP* sprite, int x, int y, int cx, int cy, fixed angle);
void pivot_scaled_sprite(BITMAP* dst, BITMAP* sprite, int x, int y, int cx, int cy,
                         fixed angle, fixed scale);
void rotate_sprite_v_flip(BITMAP* dst, BITMAP* sprite, int x, int y, fixed angle);
void pivot_sprite_v_flip(BITMAP* dst, BITMAP* sprite, int x, int y, int cx, int cy, fixed angle);
void draw_gouraud_sprite(BITMAP* dst, BITMAP* sprite, int x, int y, int c1, int c2, int c3, int c4);

void rectfill(BITMAP* bmp, int x1, int y1, int x2, int y2, int color);
void rect(BITMAP* bmp, int x1, int y1, int x2, int y2, int color);
void line(BITMAP* bmp, int x1, int y1, int x2, int y2, int color);
void fastline(BITMAP* bmp, int x1, int y1, int x2, int y2, int color);
void hline(BITMAP* bmp, int x1, int y, int x2, int color);
void vline(BITMAP* bmp, int x, int y1, int y2, int color);
void circlefill(BITMAP* bmp, int x, int y, int r, int color);
void circle(BITMAP* bmp, int x, int y, int r, int color);
void ellipse(BITMAP* bmp, int x, int y, int rx, int ry, int color);
void ellipsefill(BITMAP* bmp, int x, int y, int rx, int ry, int color);
void triangle(BITMAP* bmp, int x1, int y1, int x2, int y2, int x3, int y3, int color);
void floodfill(BITMAP* bmp, int x, int y, int color);

static inline void do_stretch_blit(BITMAP* s, BITMAP* d,
                                    int sx, int sy, int sw, int sh,
                                    int dx, int dy, int dw, int dh, int masked)
{
    if (masked) masked_stretch_blit(s, d, sx, sy, sw, sh, dx, dy, dw, dh);
    else             stretch_blit(s, d, sx, sy, sw, sh, dx, dy, dw, dh);
}

// ============================================================
// Clipping helpers
// ============================================================
static inline void set_clip_rect(BITMAP* bmp, int x1, int y1, int x2, int y2)
{
    if (!bmp) return;
    bmp->clip = 1;
    bmp->cl = x1; bmp->ct = y1;
    bmp->cr = x2; bmp->cb = y2;
}
static inline void set_clip(BITMAP* bmp, int x1, int y1, int x2, int y2)
{
    set_clip_rect(bmp, x1, y1, x2, y2);
}
static inline void set_clip_state(BITMAP* bmp, int state) { if (bmp) bmp->clip = state; }
static inline int  get_clip_state(BITMAP* bmp) { return bmp ? bmp->clip : 0; }
static inline void get_clip_rect(BITMAP* bmp, int* x1, int* y1, int* x2, int* y2)
{
    if (!bmp) return;
    if (x1) *x1 = bmp->cl;  if (y1) *y1 = bmp->ct;
    if (x2) *x2 = bmp->cr;  if (y2) *y2 = bmp->cb;
}
static inline void add_clip_rect(BITMAP* bmp, int x1, int y1, int x2, int y2)
{
    (void)bmp; (void)x1; (void)y1; (void)x2; (void)y2; /* stub */
}

// ============================================================
// Drawing mode
// ============================================================
#define DRAW_MODE_SOLID          0
#define DRAW_MODE_XOR            1
#define DRAW_MODE_COPY_PATTERN   2
#define DRAW_MODE_SOLID_PATTERN  3
#define DRAW_MODE_MASKED_PATTERN 4
#define DRAW_MODE_TRANS          5

extern int _drawing_mode;

static inline void drawing_mode(int mode, BITMAP* p, int x, int y)
{
    _drawing_mode = mode;
    (void)p; (void)x; (void)y;
}
static inline void solid_mode()  { _drawing_mode = DRAW_MODE_SOLID; }

// Color translation tables
typedef struct _COLOR_MAP { unsigned char data[32][32][32]; } COLOR_MAP;
extern COLOR_MAP* color_map;
extern COLOR_MAP* trans_map;

// ============================================================
// FONT type
// ============================================================
// Allegro 4 fonts have a vtable with char_length; we stub it so call-sites
// compile.  Full per-file migration of vtable accesses is tracked in
// PROGRESS.md under Phase 2 open items.
struct FONT;

typedef struct FONT_VTABLE {
    int  (*char_length)(struct FONT* f, int ch);
    int  (*text_length)(struct FONT* f, const char* str);
    int  (*font_height)(struct FONT* f);
    void (*render_char)(struct FONT* f, BITMAP* bmp, int ch, int x, int y, int fg, int bg);
} FONT_VTABLE;

struct FONT {
    ALLEGRO_FONT* _al5font;
    FONT_VTABLE*  vtable;
    int           height;
    // For bitmap fonts embedded in .dat files: raw data pointer
    void*         _raw;
};

extern FONT* font; // global current font

FONT* load_font(const char* filename, PALETTE pal, void* param);
void  destroy_font(FONT* f);
void  allegro_404_char(FONT* f, int x, int y, int w, int h, int fg, int bg);

// Text rendering
void textout_ex(BITMAP* bmp, const FONT* f, const char* str, int x, int y, int fg, int bg);
void textout_centre_ex(BITMAP* bmp, const FONT* f, const char* str, int x, int y, int fg, int bg);
void textout_right_ex(BITMAP* bmp, const FONT* f, const char* str, int x, int y, int fg, int bg);
void textprintf_ex(BITMAP* bmp, const FONT* f, int x, int y, int fg, int bg,
                   const char* fmt, ...);
void textprintf_centre_ex(BITMAP* bmp, const FONT* f, int x, int y, int fg, int bg,
                          const char* fmt, ...);
void textprintf_right_ex(BITMAP* bmp, const FONT* f, int x, int y, int fg, int bg,
                         const char* fmt, ...);
void textprintf_justify_ex(BITMAP* bmp, const FONT* f, int x1, int x2, int y,
                           int diff, int fg, int bg, const char* fmt, ...);
int  text_length(const FONT* f, const char* str);
int  text_height(const FONT* f);
int  font_height(const FONT* f);

// ============================================================
// Keyboard
// ============================================================
#define KEY_MAX  ALLEGRO_KEY_MAX

extern volatile char key[ALLEGRO_KEY_MAX];
extern volatile int  key_shifts;

// Allegro 4 KEY_* → Allegro 5 ALLEGRO_KEY_* mapping
#define KEY_A            ALLEGRO_KEY_A
#define KEY_B            ALLEGRO_KEY_B
#define KEY_C            ALLEGRO_KEY_C
#define KEY_D            ALLEGRO_KEY_D
#define KEY_E            ALLEGRO_KEY_E
#define KEY_F            ALLEGRO_KEY_F
#define KEY_G            ALLEGRO_KEY_G
#define KEY_H            ALLEGRO_KEY_H
#define KEY_I            ALLEGRO_KEY_I
#define KEY_J            ALLEGRO_KEY_J
#define KEY_K            ALLEGRO_KEY_K
#define KEY_L            ALLEGRO_KEY_L
#define KEY_M            ALLEGRO_KEY_M
#define KEY_N            ALLEGRO_KEY_N
#define KEY_O            ALLEGRO_KEY_O
#define KEY_P            ALLEGRO_KEY_P
#define KEY_Q            ALLEGRO_KEY_Q
#define KEY_R            ALLEGRO_KEY_R
#define KEY_S            ALLEGRO_KEY_S
#define KEY_T            ALLEGRO_KEY_T
#define KEY_U            ALLEGRO_KEY_U
#define KEY_V            ALLEGRO_KEY_V
#define KEY_W            ALLEGRO_KEY_W
#define KEY_X            ALLEGRO_KEY_X
#define KEY_Y            ALLEGRO_KEY_Y
#define KEY_Z            ALLEGRO_KEY_Z
#define KEY_0            ALLEGRO_KEY_0
#define KEY_1            ALLEGRO_KEY_1
#define KEY_2            ALLEGRO_KEY_2
#define KEY_3            ALLEGRO_KEY_3
#define KEY_4            ALLEGRO_KEY_4
#define KEY_5            ALLEGRO_KEY_5
#define KEY_6            ALLEGRO_KEY_6
#define KEY_7            ALLEGRO_KEY_7
#define KEY_8            ALLEGRO_KEY_8
#define KEY_9            ALLEGRO_KEY_9
#define KEY_0_PAD        ALLEGRO_KEY_PAD_0
#define KEY_1_PAD        ALLEGRO_KEY_PAD_1
#define KEY_2_PAD        ALLEGRO_KEY_PAD_2
#define KEY_3_PAD        ALLEGRO_KEY_PAD_3
#define KEY_4_PAD        ALLEGRO_KEY_PAD_4
#define KEY_5_PAD        ALLEGRO_KEY_PAD_5
#define KEY_6_PAD        ALLEGRO_KEY_PAD_6
#define KEY_7_PAD        ALLEGRO_KEY_PAD_7
#define KEY_8_PAD        ALLEGRO_KEY_PAD_8
#define KEY_9_PAD        ALLEGRO_KEY_PAD_9
#define KEY_F1           ALLEGRO_KEY_F1
#define KEY_F2           ALLEGRO_KEY_F2
#define KEY_F3           ALLEGRO_KEY_F3
#define KEY_F4           ALLEGRO_KEY_F4
#define KEY_F5           ALLEGRO_KEY_F5
#define KEY_F6           ALLEGRO_KEY_F6
#define KEY_F7           ALLEGRO_KEY_F7
#define KEY_F8           ALLEGRO_KEY_F8
#define KEY_F9           ALLEGRO_KEY_F9
#define KEY_F10          ALLEGRO_KEY_F10
#define KEY_F11          ALLEGRO_KEY_F11
#define KEY_F12          ALLEGRO_KEY_F12
#define KEY_ESC          ALLEGRO_KEY_ESCAPE
#define KEY_ESCAPE       ALLEGRO_KEY_ESCAPE
#define KEY_TILDE        ALLEGRO_KEY_TILDE
#define KEY_MINUS        ALLEGRO_KEY_MINUS
#define KEY_EQUALS       ALLEGRO_KEY_EQUALS
#define KEY_BACKSPACE    ALLEGRO_KEY_BACKSPACE
#define KEY_TAB          ALLEGRO_KEY_TAB
#define KEY_OPENBRACE    ALLEGRO_KEY_OPENBRACE
#define KEY_CLOSEBRACE   ALLEGRO_KEY_CLOSEBRACE
#define KEY_ENTER        ALLEGRO_KEY_ENTER
#define KEY_COLON        ALLEGRO_KEY_SEMICOLON
#define KEY_QUOTE        ALLEGRO_KEY_QUOTE
#define KEY_BACKSLASH    ALLEGRO_KEY_BACKSLASH
#define KEY_BACKSLASH2   ALLEGRO_KEY_BACKSLASH2
#define KEY_COMMA        ALLEGRO_KEY_COMMA
#define KEY_STOP         ALLEGRO_KEY_FULLSTOP
#define KEY_SLASH        ALLEGRO_KEY_SLASH
#define KEY_SPACE        ALLEGRO_KEY_SPACE
#define KEY_INSERT       ALLEGRO_KEY_INSERT
#define KEY_DEL          ALLEGRO_KEY_DELETE
#define KEY_DELETE       ALLEGRO_KEY_DELETE
#define KEY_HOME         ALLEGRO_KEY_HOME
#define KEY_END          ALLEGRO_KEY_END
#define KEY_PGUP         ALLEGRO_KEY_PGUP
#define KEY_PGDN         ALLEGRO_KEY_PGDN
#define KEY_LEFT         ALLEGRO_KEY_LEFT
#define KEY_RIGHT        ALLEGRO_KEY_RIGHT
#define KEY_UP           ALLEGRO_KEY_UP
#define KEY_DOWN         ALLEGRO_KEY_DOWN
#define KEY_SLASH_PAD    ALLEGRO_KEY_PAD_SLASH
#define KEY_ASTERISK     ALLEGRO_KEY_PAD_ASTERISK
#define KEY_MINUS_PAD    ALLEGRO_KEY_PAD_MINUS
#define KEY_PLUS_PAD     ALLEGRO_KEY_PAD_PLUS
#define KEY_DEL_PAD      ALLEGRO_KEY_PAD_DELETE
#define KEY_ENTER_PAD    ALLEGRO_KEY_PAD_ENTER
#define KEY_PRTSCR       ALLEGRO_KEY_PRINTSCREEN
#define KEY_PAUSE        ALLEGRO_KEY_PAUSE
#define KEY_LSHIFT       ALLEGRO_KEY_LSHIFT
#define KEY_RSHIFT       ALLEGRO_KEY_RSHIFT
#define KEY_LCONTROL     ALLEGRO_KEY_LCTRL
#define KEY_RCONTROL     ALLEGRO_KEY_RCTRL
#define KEY_ALT          ALLEGRO_KEY_ALT
#define KEY_ALTGR        ALLEGRO_KEY_ALTGR
#define KEY_LWIN         ALLEGRO_KEY_LWIN
#define KEY_RWIN         ALLEGRO_KEY_RWIN
#define KEY_MENU         ALLEGRO_KEY_MENU
#define KEY_SCRLOCK      ALLEGRO_KEY_SCROLLLOCK
#define KEY_NUMLOCK      ALLEGRO_KEY_NUMLOCK
#define KEY_CAPSLOCK     ALLEGRO_KEY_CAPSLOCK
#define KEY_COMMAND      ALLEGRO_KEY_COMMAND
// Allegro 4 keys with no Allegro 5 equivalent — map to 0 (never pressed)
#define KEY_ABNT_C1      0
#define KEY_YEN          0
#define KEY_KANA         0
#define KEY_CONVERT      0
#define KEY_NOCONVERT    0
#define KEY_AT           0
#define KEY_CIRCUMFLEX   0
#define KEY_COLON2       0
#define KEY_KANJI        0

// Keyboard modifier bit-flags (Allegro 4 key_shifts)
#define KB_SHIFT_FLAG    ALLEGRO_KEYMOD_SHIFT
#define KB_CTRL_FLAG     ALLEGRO_KEYMOD_CTRL
#define KB_ALT_FLAG      ALLEGRO_KEYMOD_ALT
#define KB_LWIN_FLAG     ALLEGRO_KEYMOD_LWIN
#define KB_RWIN_FLAG     ALLEGRO_KEYMOD_RWIN
#define KB_MENU_FLAG     ALLEGRO_KEYMOD_MENU
#define KB_COMMAND_FLAG  ALLEGRO_KEYMOD_COMMAND
#define KB_SCROLOCK_FLAG ALLEGRO_KEYMOD_SCROLLLOCK
#define KB_NUMLOCK_FLAG  ALLEGRO_KEYMOD_NUMLOCK
#define KB_CAPSLOCK_FLAG ALLEGRO_KEYMOD_CAPSLOCK

int  install_keyboard();
void remove_keyboard();
static inline void poll_keyboard() {}
int  keypressed();
int  readkey();
int  ureadkey(int* scancode);
void clear_keybuf();

// ============================================================
// Mouse
// ============================================================
extern volatile int mouse_x, mouse_y, mouse_z, mouse_w;
extern volatile int mouse_b;      // button bitmask (bit 0=left, 1=right, 2=middle)

int  install_mouse();
void remove_mouse();
static inline int  mouse_needs_poll()               { return 0; }
static inline void poll_mouse()                     {}
static inline void show_mouse(BITMAP* bmp)          { (void)bmp; }
static inline void hide_mouse()                     {}
static inline void scare_mouse()                    {}
static inline void unscare_mouse()                  {}
static inline void scare_mouse_area(int x, int y, int w, int h)
                                                    { (void)x;(void)y;(void)w;(void)h; }
void set_mouse_pos(int x, int y);
void set_mouse_range(int x1, int y1, int x2, int y2);
void set_mouse_speed(int xspeed, int yspeed);
int  get_mouse_mickeys(int* mickeyx, int* mickeyy);

// ============================================================
// Joystick (stub — actual joystick support deferred to Phase 4)
// ============================================================
#define JOY_TYPE_AUTODETECT (-1)
int  install_joystick(int type);
void remove_joystick();

// ============================================================
// System initialisation / display
// ============================================================
extern char allegro_error[256];
#define ALLEGRO_ERROR_SIZE 256

int  allegro_init();
void allegro_exit();
void set_window_title(const char* title);

// Graphics mode constants (Allegro 4 set_gfx_mode modes)
#define GFX_TEXT                  (-1)
#define GFX_AUTODETECT              0
#define GFX_AUTODETECT_WINDOWED     1
#define GFX_AUTODETECT_FULLSCREEN   2
#define GFX_QUARTZ_FULLSCREEN       3
#define GFX_QUARTZ_WINDOW           4
#define GFX_SAFE                    0x00000001

int  set_gfx_mode(int mode, int w, int h, int vw, int vh);
void set_color_depth(int depth);
int  get_color_depth();
int  desktop_color_depth();
int  get_desktop_resolution(int* w, int* h);
void vsync();
void rest(unsigned int ms);
void rest_callback(unsigned int ms, void (*cb)(void));

// Called each frame to drain Allegro 5 events → update key[] / mouse_* globals
void _al5_pump_events();

// Flip the screen: upload screen→_al5bmp, al_flip_display
void _al5_flip_display();

// ============================================================
// Timer
// ============================================================
int  install_timer();
void remove_timer();
int  install_int(void (*proc)(void), int msec);
int  install_int_ex(void (*proc)(void), int speed);
void remove_int(void (*proc)(void));
int  install_param_int(void (*proc)(void*), void* param, int msec);
int  install_param_int_ex(void (*proc)(void*), void* param, int speed);
void remove_param_int(void (*proc)(void*), void* param);

extern volatile int retrace_count;

// Speed conversion macros (Allegro 4 timer ticks = 1193181 Hz)
#define SECS_TO_TIMER(x)   ((int)((x)  * 1193181L))
#define MSEC_TO_TIMER(x)   ((int)((x)  * 1193.181))
#define BPS_TO_TIMER(x)    ((int)(1193181L / (x)))
#define BPM_TO_TIMER(x)    ((int)((1193181L * 60L) / (x)))

// Variable/function lock macros (no-op in Allegro 5 event model)
#define LOCK_VARIABLE(x)   ((void)0)
#define LOCK_FUNCTION(x)   ((void)0)

// High-precision clock helpers
unsigned long myclock();
long int      myclock2();

// ============================================================
// PACKFILE (file I/O — thin wrapper around ALLEGRO_FILE)
// ============================================================
typedef struct PACKFILE_COMPAT {
    ALLEGRO_FILE* _f;
} PACKFILE;

PACKFILE* pack_fopen(const char* filename, const char* mode);
void      pack_fclose(PACKFILE* f);
int       pack_feof(PACKFILE* f);
int       pack_ferror(PACKFILE* f);
int       pack_fseek(PACKFILE* f, int offset);
int       pack_getc(PACKFILE* f);
int       pack_putc(int c, PACKFILE* f);
int       pack_fread(void* buf, int size, PACKFILE* f);
int       pack_fwrite(const void* buf, int size, PACKFILE* f);
// Big-endian multi-byte I/O
int       pack_mgetw(PACKFILE* f);
int       pack_mputw(int w, PACKFILE* f);
int32_t   pack_mgetl(PACKFILE* f);
int       pack_mputl(int32_t l, PACKFILE* f);
// Little-endian multi-byte I/O
int       pack_igetw(PACKFILE* f);
int       pack_iputw(int w, PACKFILE* f);
int32_t   pack_igetl(PACKFILE* f);
int       pack_iputl(int32_t l, PACKFILE* f);

// Convenience inline aliases to match Allegro 4 non-pack_ names
static inline int16_t igetw(PACKFILE* f)          { return (int16_t)pack_igetw(f); }
static inline int32_t igetl(PACKFILE* f)          { return pack_igetl(f); }
static inline int     iputw(int w, PACKFILE* f)   { return pack_iputw(w, f); }
static inline int     iputl(int32_t l, PACKFILE* f){ return pack_iputl(l, f); }
static inline int     mgetw(PACKFILE* f)          { return pack_mgetw(f); }
static inline int32_t mgetl(PACKFILE* f)          { return pack_mgetl(f); }
static inline int     mputw(int w, PACKFILE* f)   { return pack_mputw(w, f); }
static inline int     mputl(int32_t l, PACKFILE* f){ return pack_mputl(l, f); }

// ============================================================
// File system utilities
// ============================================================
int  file_exists(const char* filename, int attrib, int* aret);
int64_t file_size_ex(const char* filename);
char* get_filename(const char* path);
char* get_extension(const char* path);
void  put_backslash(char* filename);
int   replace_filename(char* dest, const char* path, const char* filename, int size);
int   replace_extension(char* dest, const char* filename, const char* ext, int size);
void  append_filename(char* dest, const char* path, const char* filename, int size);
int   get_filepath(const char* filename, char* buf, int size);
bool  is_relative_filename(const char* filename);

// ============================================================
// Allegro 4 GUI / DIALOG system
// ============================================================
// These structs and constants are reproduced verbatim from Allegro 4's
// allegro/gui.h.  The jwin.cpp GUI layer depends on them.  The dialog
// runner functions are stubbed in Phase 2 and need a full Allegro-5-native
// replacement in Phase 4.
typedef int (*DIALOG_PROC)(int msg, struct DIALOG* d, int c);

typedef struct DIALOG {
    DIALOG_PROC proc;
    int x, y, w, h;
    int fg, bg;
    int key;
    int flags;
    int d1, d2;
    void* dp;
    void* dp2;
    void* dp3;
} DIALOG;

typedef struct MENU {
    const char* text;
    DIALOG_PROC proc;
    struct MENU* child;
    int flags;
    void* dp;
} MENU;

typedef struct DIALOG_PLAYER {
    int focus_obj;
    int mouse_obj;
    int joy_obj;
    int key_obj;
    int res;
    int redraw;
    DIALOG* dialog;
} DIALOG_PLAYER;

typedef struct MENU_PLAYER {
    MENU* menu;
    int bar;
    int active_item;
    int mouse_item;
    int redraw;
    int sel;
    struct MENU_PLAYER* parent;
    struct MENU_PLAYER* child;
    DIALOG_PLAYER* player;
} MENU_PLAYER;

// DIALOG flags
#define D_EXIT          1
#define D_SELECTED      2
#define D_GOTFOCUS      4
#define D_GOTMOUSE      8
#define D_HIDDEN        16
#define D_DISABLED      32
#define D_DIRTY         64
#define D_INTERNAL      128
#define D_USER          256

// DIALOG return codes
#define D_O_K           0
#define D_CLOSE         1
#define D_REDRAW        2
#define D_REDRAWME      4
#define D_WANTFOCUS     8
#define D_USED_CHAR     16
#define D_REDRAW_ALL    32
#define D_DONTWANTMOUSE 64

// DIALOG messages
#define MSG_START       1
#define MSG_END         2
#define MSG_DRAW        3
#define MSG_CLICK       4
#define MSG_DCLICK      5
#define MSG_KEY         6
#define MSG_CHAR        7
#define MSG_UCHAR       8
#define MSG_XCHAR       9
#define MSG_WANTFOCUS   10
#define MSG_GOTFOCUS    11
#define MSG_LOSTFOCUS   12
#define MSG_GOTMOUSE    13
#define MSG_LOSTMOUSE   14
#define MSG_WHEEL       15
#define MSG_LPRESS      16
#define MSG_LRELEASE    17
#define MSG_MPRESS      18
#define MSG_MRELEASE    19
#define MSG_RPRESS      20
#define MSG_RRELEASE    21
#define MSG_JOYMOVE     22
#define MSG_JOYB1       23
#define MSG_JOYB2       24
#define MSG_IDLE        25
#define MSG_RADIO       26
#define MSG_WHEEL2      27
#define MSG_USER        128

// Dialog runner functions (Phase 2 stubs; proper implementation in Phase 4)
int           do_dialog(DIALOG* dialog, int focus_obj);
int           popup_dialog(DIALOG* dialog, int focus_obj);
DIALOG_PLAYER* init_dialog(DIALOG* dialog, int focus_obj);
int           update_dialog(DIALOG_PLAYER* player);
int           shutdown_dialog(DIALOG_PLAYER* player);
int           do_menu(MENU* menu, int x, int y);
MENU_PLAYER*  init_menu(MENU_PLAYER* player, MENU* menu);
int           update_menu(MENU_PLAYER* player);
int           shutdown_menu(MENU_PLAYER* player);

int           alert(const char* s1, const char* s2, const char* s3,
                    const char* b1, const char* b2, int c1, int c2);
int           alert3(const char* s1, const char* s2, const char* s3,
                     const char* b1, const char* b2, const char* b3,
                     int c1, int c2, int c3);
int           file_select_ex(const char* message, char* path, const char* ext,
                             int size, int w, int h);

// Allegro 4 broadcast_dialog_message helper
int broadcast_dialog_message(int msg, int c);

// ============================================================
// Configuration system
// ============================================================
void set_config_file(const char* filename);
void set_config_data(const char* data, int length);
void push_config_state();
void pop_config_state();
const char* get_config_string(const char* section, const char* name, const char* def);
int         get_config_int(const char* section, const char* name, int def);
float       get_config_float(const char* section, const char* name, float def);
int         get_config_hex(const char* section, const char* name, int def);
void set_config_string(const char* section, const char* name, const char* val);
void set_config_int(const char* section, const char* name, int val);
void set_config_float(const char* section, const char* name, float val);
void set_config_hex(const char* section, const char* name, int val);
// Override: set a config value without writing to disk
void override_config_string(const char* section, const char* name, const char* str);

// ============================================================
// Datafile
// ============================================================
typedef struct DATAFILE {
    void* dat;
    int   type;
    long  size;
    void* prop;
} DATAFILE;

#define DAT_MAGIC    0x616C6C2EL
#define DAT_FILE     DAT_MAGIC
#define DAT_BITMAP   0x424D5020L
#define DAT_FONT     0x464F4E20L
#define DAT_SAMPLE   0x53414D20L
#define DAT_MIDI     0x4D494420L
#define DAT_PALETTE  0x50414C20L
#define DAT_END      -1

DATAFILE* load_datafile(const char* filename);
void      unload_datafile(DATAFILE* dat);
DATAFILE* find_datafile_object(DATAFILE* dat, const char* objectname);
DATAFILE* load_datafile_object(const char* filename, const char* objectname);
void      unload_datafile_object(DATAFILE* dat);

// ============================================================
// Sound stubs (placeholder until Phase 5 / zcsound)
// ============================================================
#define DIGI_AUTODETECT  (-1)
#define DIGI_NONE        (-2)
#define MIDI_AUTODETECT  (-1)
#define MIDI_NONE        (-2)

int  install_sound(int digi, int midi, const char* cfg);
void remove_sound();

typedef struct SAMPLE { ALLEGRO_SAMPLE* _s; } SAMPLE;

SAMPLE* load_sample(const char* filename);
void    destroy_sample(SAMPLE* s);
void    play_sample(SAMPLE* s, int vol, int pan, int freq, int loop);
void    stop_sample(SAMPLE* s);
void    adjust_sample(SAMPLE* s, int vol, int pan, int freq, int loop);

typedef struct MIDI { int length; } MIDI;
MIDI* load_midi(const char* filename);
void  destroy_midi(MIDI* midi);
int   play_midi(MIDI* midi, int loop);
void  stop_midi();
void  midi_pause();
void  midi_resume();
int   midi_pos;   // current MIDI position (Allegro 4 global)

// ============================================================
// CPU / system info
// ============================================================
extern int  cpu_family;
extern int  cpu_model;
extern int  cpu_capabilities;
extern char cpu_vendor[256];

#define CPU_MMX    (1 << 0)
#define CPU_SSE    (1 << 3)
#define CPU_SSE2   (1 << 4)
#define CPU_3DNOW  (1 << 5)

// ============================================================
// Unicode / string helpers (Allegro 4 u* functions)
// ============================================================
// These are thin wrappers around standard C functions since we always
// operate in UTF-8 / ASCII in this codebase.
static inline int  ustrlen(const char* s)                    { return (int)strlen(s); }
static inline int  ustrcmp(const char* a, const char* b)     { return strcmp(a, b); }
static inline int  ustrncmp(const char* a, const char* b, int n) { return strncmp(a, b, n); }
static inline char* ustrtok(char* s, const char* d)          { return strtok(s, d); }
static inline int  ugetc(const char* s)                      { return (unsigned char)*s; }
static inline int  usetc(char* s, int c)                     { *s = (char)c; return 1; }
static inline int  uwidth(const char* s)                     { return (int)strlen(s); }
static inline int  unshift(char* s, int c)                   { (void)s; (void)c; return 0; }

// ============================================================
// Misc Allegro 4 globals / helpers
// ============================================================
extern int os_type;       // operating system type
extern int os_version;
extern int os_revision;
extern int os_multitasking;

#define OSTYPE_MACOSX    0x4D414358  // 'MACX'

#endif // _ALLEGRO5_COMPAT_H_
