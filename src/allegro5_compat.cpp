// allegro5_compat.cpp — Allegro 4 → Allegro 5 compatibility layer (implementation)
//
// macOS (__APPLE__) builds only.  See allegro5_compat.h for design notes.

#ifdef __APPLE__

#include "allegro5_compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <dirent.h>
#include <cmath>
#include <algorithm>

// ============================================================
// Global state
// ============================================================
PALETTE _zc_palette;
bool    _zc_palette_dirty = true;

static ALLEGRO_COLOR _color_cache[256];

ALLEGRO_DISPLAY*     _al5_display     = nullptr;
ALLEGRO_EVENT_QUEUE* _al5_event_queue = nullptr;
ALLEGRO_TIMER*       _al5_timer       = nullptr;

BITMAP* screen = nullptr;

int _zc_screen_w = 0;
int _zc_screen_h = 0;

volatile char key[ALLEGRO_KEY_MAX];
volatile int  key_shifts = 0;
volatile int  mouse_x = 0, mouse_y = 0, mouse_z = 0, mouse_w = 0;
volatile int  mouse_b = 0;
volatile int  retrace_count = 0;

int _drawing_mode = DRAW_MODE_SOLID;

COLOR_MAP* color_map  = nullptr;
COLOR_MAP* trans_map  = nullptr;

FONT* font = nullptr;

char allegro_error[256] = {0};

int  cpu_family       = 6;
int  cpu_model        = 0;
int  cpu_capabilities = 0;
char cpu_vendor[256]  = "Apple";

int os_type        = OSTYPE_MACOSX;
int os_version     = 0;
int os_revision    = 0;
int os_multitasking = 1;

// Allegro 5 config (replaces Allegro 4 set_config_file / get_config_*)
static ALLEGRO_CONFIG* _al5_config = nullptr;
static char _al5_config_path[4096] = {0};

// Key event ring-buffer for readkey()
static const int KEYBUF_SIZE = 256;
static int _keybuf[KEYBUF_SIZE];
static int _keybuf_head = 0, _keybuf_tail = 0;

// Installed timer callbacks
struct TimerCallback { void (*proc)(void); int period_us; int64_t next_us; };
static std::vector<TimerCallback> _timer_callbacks;

static int64_t _us_now()
{
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return (int64_t)tv.tv_sec * 1000000LL + tv.tv_usec;
}

// ============================================================
// Palette
// ============================================================
void _al5_rebuild_color_cache()
{
    for (int i = 0; i < 256; i++) {
        _color_cache[i] = al_map_rgb(_zc_palette[i].r,
                                     _zc_palette[i].g,
                                     _zc_palette[i].b);
    }
    _zc_palette_dirty = false;
}

ALLEGRO_COLOR _al5_color(int idx)
{
    if (_zc_palette_dirty) _al5_rebuild_color_cache();
    if (idx >= 0 && idx < 256) return _color_cache[idx];
    // Packed direct-RGB (from makecol32 etc.): decode r,g,b
    int r = (idx >> 16) & 0xFF;
    int g = (idx >>  8) & 0xFF;
    int b =  idx        & 0xFF;
    return al_map_rgb(r, g, b);
}

int _al5_bestfit_color(int r, int g, int b)
{
    // Find nearest palette entry by squared Euclidean distance in RGB space.
    int best = 0, best_dist = INT32_MAX;
    for (int i = 0; i < 256; i++) {
        int dr = (int)_zc_palette[i].r - r;
        int dg = (int)_zc_palette[i].g - g;
        int db = (int)_zc_palette[i].b - b;
        int dist = dr*dr + dg*dg + db*db;
        if (dist < best_dist) { best_dist = dist; best = i; }
        if (dist == 0) break;
    }
    return best;
}

void set_palette(const PALETTE p)
{
    memcpy(_zc_palette, p, sizeof(PALETTE));
    _zc_palette_dirty = true;
}

void get_palette(PALETTE p)
{
    memcpy(p, _zc_palette, sizeof(PALETTE));
}

void set_color(int idx, const RGB* rgb)
{
    if (idx < 0 || idx >= 256 || !rgb) return;
    _zc_palette[idx] = *rgb;
    _zc_palette_dirty = true;
}

void get_color(int idx, RGB* rgb)
{
    if (idx < 0 || idx >= 256 || !rgb) return;
    *rgb = _zc_palette[idx];
}

// ============================================================
// BITMAP
// ============================================================
void BITMAP::_rebuild_line_ptrs()
{
    if (!_data || h <= 0) { line = nullptr; return; }
    // The line-pointer array is stored right after the BITMAP pixel data.
    // We allocate it separately as a std::vector, but for simplicity here
    // we heap-allocate alongside the data.  Phase 2: simple flat allocation.
    delete[] line;
    line = new uint8_t*[h];
    for (int y = 0; y < h; y++)
        line[y] = _data + y * w;
}

BITMAP::BITMAP(int _w, int _h)
    : w(_w), h(_h)
    , clip(1), cl(0), cr(_w), ct(0), cb(_h)
    , line(nullptr)
    , _data(nullptr)
    , _al5bmp(nullptr)
    , _owns_al5bmp(false)
{
    if (w > 0 && h > 0) {
        _data = new uint8_t[w * h]();  // zero-initialised
        _rebuild_line_ptrs();
    }
}

BITMAP::~BITMAP()
{
    delete[] _data;  _data = nullptr;
    delete[] line;   line  = nullptr;
    if (_al5bmp && _owns_al5bmp) {
        al_destroy_bitmap(_al5bmp);
        _al5bmp = nullptr;
    }
}

// Upload 8-bit palette data to _al5bmp (create the A5 bitmap if needed).
void BITMAP::upload_to_al5()
{
    if (!_data) return;
    if (!_al5bmp) {
        ALLEGRO_BITMAP* old_target = al_get_target_bitmap();
        al_set_new_bitmap_flags(ALLEGRO_VIDEO_BITMAP);
        _al5bmp = al_create_bitmap(w, h);
        _owns_al5bmp = true;
        al_set_target_bitmap(old_target);
        if (!_al5bmp) return;
    }

    if (_zc_palette_dirty) _al5_rebuild_color_cache();

    ALLEGRO_LOCKED_REGION* region = al_lock_bitmap(
        _al5bmp, ALLEGRO_PIXEL_FORMAT_ABGR_8888_LE, ALLEGRO_LOCK_WRITEONLY);
    if (!region) return;

    for (int y = 0; y < h; y++) {
        uint32_t* dst = reinterpret_cast<uint32_t*>(
            reinterpret_cast<uint8_t*>(region->data) + y * region->pitch);
        uint8_t*  src = _data + y * w;
        for (int x = 0; x < w; x++) {
            ALLEGRO_COLOR c = _color_cache[src[x]];
            unsigned char r, g, b, a;
            al_unmap_rgba(c, &r, &g, &b, &a);
            // ABGR_8888_LE: byte order A-B-G-R in memory → little-endian RGBA
            dst[x] = ((uint32_t)0xFF << 24) |
                     ((uint32_t)b   << 16) |
                     ((uint32_t)g   <<  8) |
                     ((uint32_t)r);
        }
    }
    al_unlock_bitmap(_al5bmp);
}

// ============================================================
// Bitmap construction helpers
// ============================================================
BITMAP* create_bitmap(int w, int h)
{
    if (w <= 0 || h <= 0) return nullptr;
    return new BITMAP(w, h);
}

BITMAP* create_bitmap_ex(int bpp, int w, int h)
{
    (void)bpp;  // we always use 8-bit internal
    return create_bitmap(w, h);
}

BITMAP* create_video_bitmap(int w, int h)
{
    // In the compat layer, video bitmaps behave like memory bitmaps.
    return create_bitmap(w, h);
}

BITMAP* create_system_bitmap(int w, int h)
{
    return create_bitmap(w, h);
}

BITMAP* create_sub_bitmap(BITMAP* parent, int x, int y, int w, int h)
{
    // A proper sub-bitmap shares the parent's pixel data.  For Phase 2,
    // create an independent copy and note it as a known limitation.
    if (!parent || w <= 0 || h <= 0) return nullptr;
    BITMAP* sub = new BITMAP(w, h);
    // Copy the region from the parent
    for (int row = 0; row < h; row++) {
        int py = y + row;
        if (py < 0 || py >= parent->h) continue;
        for (int col = 0; col < w; col++) {
            int px = x + col;
            if (px < 0 || px >= parent->w) continue;
            sub->line[row][col] = parent->line[py][px];
        }
    }
    return sub;
}

void destroy_bitmap(BITMAP* bmp)
{
    if (!bmp) return;
    if (bmp == screen) screen = nullptr;
    delete bmp;
}

// ============================================================
// Drawing primitives
// ============================================================
void clear_bitmap(BITMAP* bmp)
{
    if (!bmp || !bmp->_data) return;
    memset(bmp->_data, 0, bmp->w * bmp->h);
}

void clear_to_color(BITMAP* bmp, int color)
{
    if (!bmp || !bmp->_data) return;
    memset(bmp->_data, (uint8_t)color, bmp->w * bmp->h);
}

void blit(BITMAP* src, BITMAP* dst, int sx, int sy, int dx, int dy, int bw, int bh)
{
    if (!src || !dst) return;
    // Clip against source bounds
    if (sx < 0) { dx -= sx; bw += sx; sx = 0; }
    if (sy < 0) { dy -= sy; bh += sy; sy = 0; }
    if (sx + bw > src->w) bw = src->w - sx;
    if (sy + bh > src->h) bh = src->h - sy;
    // Clip against destination bounds / clip rect
    if (dst->clip) {
        if (dx < dst->cl) { sx += dst->cl - dx; bw -= dst->cl - dx; dx = dst->cl; }
        if (dy < dst->ct) { sy += dst->ct - dy; bh -= dst->ct - dy; dy = dst->ct; }
        if (dx + bw > dst->cr) bw = dst->cr - dx;
        if (dy + bh > dst->cb) bh = dst->cb - dy;
    } else {
        if (dx < 0) { sx -= dx; bw += dx; dx = 0; }
        if (dy < 0) { sy -= dy; bh += dy; dy = 0; }
        if (dx + bw > dst->w) bw = dst->w - dx;
        if (dy + bh > dst->h) bh = dst->h - dy;
    }
    if (bw <= 0 || bh <= 0) return;

    for (int row = 0; row < bh; row++)
        memcpy(dst->line[dy + row] + dx, src->line[sy + row] + sx, bw);
}

void masked_blit(BITMAP* src, BITMAP* dst, int sx, int sy, int dx, int dy, int bw, int bh)
{
    if (!src || !dst) return;
    // Clip source
    if (sx < 0) { dx -= sx; bw += sx; sx = 0; }
    if (sy < 0) { dy -= sy; bh += sy; sy = 0; }
    if (sx + bw > src->w) bw = src->w - sx;
    if (sy + bh > src->h) bh = src->h - sy;
    // Clip dest
    if (dst->clip) {
        if (dx < dst->cl) { sx += dst->cl - dx; bw -= dst->cl - dx; dx = dst->cl; }
        if (dy < dst->ct) { sy += dst->ct - dy; bh -= dst->ct - dy; dy = dst->ct; }
        if (dx + bw > dst->cr) bw = dst->cr - dx;
        if (dy + bh > dst->cb) bh = dst->cb - dy;
    } else {
        if (dx < 0) { sx -= dx; bw += dx; dx = 0; }
        if (dy < 0) { sy -= dy; bh += dy; dy = 0; }
        if (dx + bw > dst->w) bw = dst->w - dx;
        if (dy + bh > dst->h) bh = dst->h - dy;
    }
    if (bw <= 0 || bh <= 0) return;

    const int mask = bitmap_mask_color(src); // = 0
    for (int row = 0; row < bh; row++) {
        uint8_t* s = src->line[sy + row] + sx;
        uint8_t* d = dst->line[dy + row] + dx;
        for (int col = 0; col < bw; col++) {
            if (s[col] != mask) d[col] = s[col];
        }
    }
}

void stretch_blit(BITMAP* src, BITMAP* dst,
                  int sx, int sy, int sw, int sh,
                  int dx, int dy, int dw, int dh)
{
    if (!src || !dst || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
    for (int y = 0; y < dh; y++) {
        int ny = dy + y;
        if (ny < 0 || ny >= dst->h) continue;
        if (dst->clip && (ny < dst->ct || ny >= dst->cb)) continue;
        int srcY = sy + (y * sh) / dh;
        if (srcY < 0 || srcY >= src->h) continue;
        for (int x = 0; x < dw; x++) {
            int nx = dx + x;
            if (nx < 0 || nx >= dst->w) continue;
            if (dst->clip && (nx < dst->cl || nx >= dst->cr)) continue;
            int srcX = sx + (x * sw) / dw;
            if (srcX < 0 || srcX >= src->w) continue;
            dst->line[ny][nx] = src->line[srcY][srcX];
        }
    }
}

void masked_stretch_blit(BITMAP* src, BITMAP* dst,
                         int sx, int sy, int sw, int sh,
                         int dx, int dy, int dw, int dh)
{
    if (!src || !dst || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
    const int mask = bitmap_mask_color(src);
    for (int y = 0; y < dh; y++) {
        int ny = dy + y;
        if (ny < 0 || ny >= dst->h) continue;
        if (dst->clip && (ny < dst->ct || ny >= dst->cb)) continue;
        int srcY = sy + (y * sh) / dh;
        if (srcY < 0 || srcY >= src->h) continue;
        for (int x = 0; x < dw; x++) {
            int nx = dx + x;
            if (nx < 0 || nx >= dst->w) continue;
            if (dst->clip && (nx < dst->cl || nx >= dst->cr)) continue;
            int srcX = sx + (x * sw) / dw;
            if (srcX < 0 || srcX >= src->w) continue;
            uint8_t c = src->line[srcY][srcX];
            if (c != mask) dst->line[ny][nx] = c;
        }
    }
}

void draw_sprite(BITMAP* dst, BITMAP* sprite, int x, int y)
{
    masked_blit(sprite, dst, 0, 0, x, y, sprite->w, sprite->h);
}

void draw_sprite_v_flip(BITMAP* dst, BITMAP* sprite, int x, int y)
{
    if (!dst || !sprite) return;
    const int mask = bitmap_mask_color(sprite);
    for (int row = 0; row < sprite->h; row++) {
        int dy = y + (sprite->h - 1 - row);
        if (dy < 0 || dy >= dst->h) continue;
        if (dst->clip && (dy < dst->ct || dy >= dst->cb)) continue;
        uint8_t* s = sprite->line[row];
        uint8_t* d = dst->line[dy];
        for (int col = 0; col < sprite->w; col++) {
            int dx = x + col;
            if (dx < 0 || dx >= dst->w) continue;
            if (dst->clip && (dx < dst->cl || dx >= dst->cr)) continue;
            if (s[col] != mask) d[dx] = s[col];
        }
    }
}

void draw_sprite_h_flip(BITMAP* dst, BITMAP* sprite, int x, int y)
{
    if (!dst || !sprite) return;
    const int mask = bitmap_mask_color(sprite);
    for (int row = 0; row < sprite->h; row++) {
        int dy = y + row;
        if (dy < 0 || dy >= dst->h) continue;
        if (dst->clip && (dy < dst->ct || dy >= dst->cb)) continue;
        uint8_t* s = sprite->line[row];
        uint8_t* d = dst->line[dy];
        for (int col = 0; col < sprite->w; col++) {
            int dx = x + (sprite->w - 1 - col);
            if (dx < 0 || dx >= dst->w) continue;
            if (dst->clip && (dx < dst->cl || dx >= dst->cr)) continue;
            if (s[col] != mask) d[dx] = s[col];
        }
    }
}

void draw_sprite_vh_flip(BITMAP* dst, BITMAP* sprite, int x, int y)
{
    if (!dst || !sprite) return;
    const int mask = bitmap_mask_color(sprite);
    for (int row = 0; row < sprite->h; row++) {
        int dy = y + (sprite->h - 1 - row);
        if (dy < 0 || dy >= dst->h) continue;
        if (dst->clip && (dy < dst->ct || dy >= dst->cb)) continue;
        uint8_t* s = sprite->line[row];
        uint8_t* d = dst->line[dy];
        for (int col = 0; col < sprite->w; col++) {
            int dx = x + (sprite->w - 1 - col);
            if (dx < 0 || dx >= dst->w) continue;
            if (dst->clip && (dx < dst->cl || dx >= dst->cr)) continue;
            if (s[col] != mask) d[dx] = s[col];
        }
    }
}

void draw_trans_sprite(BITMAP* dst, BITMAP* sprite, int x, int y)
{
    // Without a trans_map, fall back to masked blit
    masked_blit(sprite, dst, 0, 0, x, y, sprite->w, sprite->h);
}

void draw_lit_sprite(BITMAP* dst, BITMAP* sprite, int x, int y, int color)
{
    // Tinting not supported in Phase 2; fall back to masked blit
    (void)color;
    masked_blit(sprite, dst, 0, 0, x, y, sprite->w, sprite->h);
}

void stretch_sprite(BITMAP* dst, BITMAP* sprite, int x, int y, int w, int h)
{
    masked_stretch_blit(sprite, dst, 0, 0, sprite->w, sprite->h, x, y, w, h);
}

void masked_stretch_sprite(BITMAP* dst, BITMAP* sprite, int x, int y, int w, int h)
{
    masked_stretch_blit(sprite, dst, 0, 0, sprite->w, sprite->h, x, y, w, h);
}

// Rotation stubs — proper implementation requires scanline rasteriser (Phase 2 TODO)
void rotate_sprite(BITMAP* dst, BITMAP* sprite, int x, int y, fixed angle)
{
    (void)angle;
    draw_sprite(dst, sprite, x, y);
}
void rotate_scaled_sprite(BITMAP* dst, BITMAP* sprite, int x, int y, fixed angle, fixed scale)
{
    (void)angle; (void)scale;
    draw_sprite(dst, sprite, x, y);
}
void pivot_sprite(BITMAP* dst, BITMAP* sprite, int x, int y, int cx, int cy, fixed angle)
{
    (void)cx; (void)cy; (void)angle;
    draw_sprite(dst, sprite, x - sprite->w/2, y - sprite->h/2);
}
void pivot_scaled_sprite(BITMAP* dst, BITMAP* sprite, int x, int y,
                         int cx, int cy, fixed angle, fixed scale)
{
    (void)cx; (void)cy; (void)angle; (void)scale;
    draw_sprite(dst, sprite, x - sprite->w/2, y - sprite->h/2);
}
void rotate_sprite_v_flip(BITMAP* dst, BITMAP* sprite, int x, int y, fixed angle)
{
    (void)angle;
    draw_sprite_v_flip(dst, sprite, x, y);
}
void pivot_sprite_v_flip(BITMAP* dst, BITMAP* sprite, int x, int y,
                         int cx, int cy, fixed angle)
{
    (void)cx; (void)cy; (void)angle;
    draw_sprite_v_flip(dst, sprite, x - sprite->w/2, y - sprite->h/2);
}
void draw_gouraud_sprite(BITMAP* dst, BITMAP* sprite, int x, int y,
                         int c1, int c2, int c3, int c4)
{
    (void)c1; (void)c2; (void)c3; (void)c4;
    draw_sprite(dst, sprite, x, y);
}

// ---- Filled rectangle ----
void rectfill(BITMAP* bmp, int x1, int y1, int x2, int y2, int color)
{
    if (!bmp) return;
    int cl = bmp->clip ? bmp->cl : 0;
    int ct = bmp->clip ? bmp->ct : 0;
    int cr = bmp->clip ? bmp->cr : bmp->w;
    int cb = bmp->clip ? bmp->cb : bmp->h;
    int lx = std::max(x1, cl);
    int rx = std::min(x2, cr - 1);
    int ty = std::max(y1, ct);
    int by = std::min(y2, cb - 1);
    if (lx > rx || ty > by) return;
    int len = rx - lx + 1;
    uint8_t c = (uint8_t)color;
    for (int y = ty; y <= by; y++)
        memset(bmp->line[y] + lx, c, len);
}

// ---- Unfilled rectangle ----
void rect(BITMAP* bmp, int x1, int y1, int x2, int y2, int color)
{
    hline(bmp, x1, y1, x2, color);
    hline(bmp, x1, y2, x2, color);
    vline(bmp, x1, y1, y2, color);
    vline(bmp, x2, y1, y2, color);
}

// ---- Horizontal / vertical lines ----
void hline(BITMAP* bmp, int x1, int y, int x2, int color)
{
    if (!bmp || y < 0 || y >= bmp->h) return;
    if (bmp->clip && (y < bmp->ct || y >= bmp->cb)) return;
    int cl = bmp->clip ? bmp->cl : 0;
    int cr = bmp->clip ? bmp->cr : bmp->w;
    int lx = std::max(std::min(x1, x2), cl);
    int rx = std::min(std::max(x1, x2), cr - 1);
    if (lx > rx) return;
    memset(bmp->line[y] + lx, (uint8_t)color, rx - lx + 1);
}

void vline(BITMAP* bmp, int x, int y1, int y2, int color)
{
    if (!bmp || x < 0 || x >= bmp->w) return;
    if (bmp->clip && (x < bmp->cl || x >= bmp->cr)) return;
    int ct = bmp->clip ? bmp->ct : 0;
    int cb = bmp->clip ? bmp->cb : bmp->h;
    int ty = std::max(std::min(y1, y2), ct);
    int by = std::min(std::max(y1, y2), cb - 1);
    for (int y = ty; y <= by; y++)
        bmp->line[y][x] = (uint8_t)color;
}

// ---- Arbitrary line (Bresenham) ----
void line(BITMAP* bmp, int x1, int y1, int x2, int y2, int color)
{
    int dx = std::abs(x2 - x1);
    int dy = std::abs(y2 - y1);
    int sx = x1 < x2 ? 1 : -1;
    int sy = y1 < y2 ? 1 : -1;
    int err = dx - dy;
    while (true) {
        putpixel(bmp, x1, y1, color);
        if (x1 == x2 && y1 == y2) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x1 += sx; }
        if (e2 <  dx) { err += dx; y1 += sy; }
    }
}

void fastline(BITMAP* bmp, int x1, int y1, int x2, int y2, int color)
{
    line(bmp, x1, y1, x2, y2, color);
}

// ---- Circles ----
void circlefill(BITMAP* bmp, int cx, int cy, int r, int color)
{
    for (int y = -r; y <= r; y++) {
        int x = (int)sqrt((double)(r*r - y*y));
        hline(bmp, cx - x, cy + y, cx + x, color);
    }
}

void circle(BITMAP* bmp, int cx, int cy, int r, int color)
{
    int x = 0, y = r, d = 3 - 2*r;
    while (y >= x) {
        putpixel(bmp, cx+x, cy+y, color);  putpixel(bmp, cx-x, cy+y, color);
        putpixel(bmp, cx+x, cy-y, color);  putpixel(bmp, cx-x, cy-y, color);
        putpixel(bmp, cx+y, cy+x, color);  putpixel(bmp, cx-y, cy+x, color);
        putpixel(bmp, cx+y, cy-x, color);  putpixel(bmp, cx-y, cy-x, color);
        if (d < 0) d += 4*x + 6;
        else       { d += 4*(x-y) + 10; y--; }
        x++;
    }
}

void ellipse(BITMAP* bmp, int cx, int cy, int rx, int ry, int color)
{
    // Midpoint ellipse algorithm
    long x = 0, y = ry;
    long rx2 = (long)rx*rx, ry2 = (long)ry*ry;
    long twoRx2 = 2*rx2, twoRy2 = 2*ry2;
    long p, px = 0, py = twoRx2 * y;

    auto put4 = [&]() {
        putpixel(bmp, (int)(cx+x), (int)(cy+y), color);
        putpixel(bmp, (int)(cx-x), (int)(cy+y), color);
        putpixel(bmp, (int)(cx+x), (int)(cy-y), color);
        putpixel(bmp, (int)(cx-x), (int)(cy-y), color);
    };

    put4();
    p = (long)(ry2 - rx2*ry + 0.25*rx2);
    while (px < py) {
        x++; px += twoRy2;
        if (p < 0) p += ry2 + px;
        else       { y--; py -= twoRx2; p += ry2 + px - py; }
        put4();
    }
    p = (long)(ry2*(x+0.5)*(x+0.5) + rx2*(y-1)*(y-1) - rx2*ry2);
    while (y > 0) {
        y--; py -= twoRx2;
        if (p > 0) p += rx2 - py;
        else       { x++; px += twoRy2; p += rx2 - py + px; }
        put4();
    }
}

void ellipsefill(BITMAP* bmp, int cx, int cy, int rx, int ry, int color)
{
    for (int y = -ry; y <= ry; y++) {
        double ratio = (double)y / ry;
        int x = (int)(rx * sqrt(1.0 - ratio*ratio));
        hline(bmp, cx - x, cy + y, cx + x, color);
    }
}

void triangle(BITMAP* bmp, int x1, int y1, int x2, int y2, int x3, int y3, int color)
{
    line(bmp, x1, y1, x2, y2, color);
    line(bmp, x2, y2, x3, y3, color);
    line(bmp, x3, y3, x1, y1, color);
}

void floodfill(BITMAP* bmp, int x, int y, int color)
{
    // Simple 4-connected stack-based flood fill
    if (!bmp || x < 0 || x >= bmp->w || y < 0 || y >= bmp->h) return;
    int target = getpixel(bmp, x, y);
    if (target == color) return;

    std::vector<std::pair<int,int>> stack;
    stack.push_back({x, y});
    while (!stack.empty()) {
        auto [px, py] = stack.back(); stack.pop_back();
        if (px < 0 || px >= bmp->w || py < 0 || py >= bmp->h) continue;
        if (bmp->line[py][px] != target) continue;
        bmp->line[py][px] = (uint8_t)color;
        stack.push_back({px+1, py});
        stack.push_back({px-1, py});
        stack.push_back({px, py+1});
        stack.push_back({px, py-1});
    }
}

// ============================================================
// Fixed-point math
// ============================================================
fixed fixsin(fixed x) { return ftofix(sin(fixtof(x) * M_PI / 128.0)); }
fixed fixcos(fixed x) { return ftofix(cos(fixtof(x) * M_PI / 128.0)); }
fixed fixtan(fixed x) { return ftofix(tan(fixtof(x) * M_PI / 128.0)); }
fixed fixasin(fixed x){ return ftofix(asin(fixtof(x)) * 128.0 / M_PI); }
fixed fixacos(fixed x){ return ftofix(acos(fixtof(x)) * 128.0 / M_PI); }
fixed fixatan(fixed x){ return ftofix(atan(fixtof(x)) * 128.0 / M_PI); }
fixed fixatan2(fixed y, fixed x){ return ftofix(atan2(fixtof(y),fixtof(x))*128.0/M_PI); }
fixed fixsqrt(fixed x){ return ftofix(sqrt(fixtof(x))); }
fixed fixhypot(fixed a, fixed b){ return ftofix(hypot(fixtof(a),fixtof(b))); }

// ============================================================
// FONT / text rendering
// ============================================================
// Stub vtable implementation functions
static int _stub_char_length(FONT* f, int ch) {
    if (!f || !f->_al5font) return 8;
    char buf[8];
    int len;
    buf[0] = (char)ch; buf[1] = 0;
    return al_get_text_width(f->_al5font, buf);
    (void)len;
}
static int _stub_text_length(FONT* f, const char* str) {
    if (!f || !f->_al5font) return 0;
    return al_get_text_width(f->_al5font, str);
}
static int _stub_font_height(FONT* f) {
    if (!f || !f->_al5font) return 8;
    return al_get_font_line_height(f->_al5font);
}
static void _stub_render_char(FONT* f, BITMAP* bmp, int ch, int x, int y, int fg, int bg) {
    char buf[8]; buf[0] = (char)ch; buf[1] = 0;
    textout_ex(bmp, f, buf, x, y, fg, bg);
}

static FONT_VTABLE _default_vtable = {
    _stub_char_length,
    _stub_text_length,
    _stub_font_height,
    _stub_render_char
};

FONT* load_font(const char* filename, PALETTE pal, void* param)
{
    (void)pal; (void)param;
    // Phase 2: attempt to load a TTF or bitmap font via Allegro 5
    ALLEGRO_FONT* af = al_load_font(filename, -12, 0);
    if (!af) return nullptr;
    FONT* f = new FONT();
    f->_al5font = af;
    f->vtable   = &_default_vtable;
    f->height   = al_get_font_line_height(af);
    f->_raw     = nullptr;
    return f;
}

void destroy_font(FONT* f)
{
    if (!f) return;
    if (f->_al5font) al_destroy_font(f->_al5font);
    delete f;
}

void allegro_404_char(FONT* f, int x, int y, int w, int h, int fg, int bg)
{
    // Draw a small placeholder rectangle
    (void)f;
    // We don't have a bitmap reference here — this is a known limitation.
    (void)x; (void)y; (void)w; (void)h; (void)fg; (void)bg;
}

int text_length(const FONT* f, const char* str)
{
    if (!f || !f->_al5font || !str) return 0;
    return al_get_text_width(f->_al5font, str);
}

int text_height(const FONT* f)
{
    if (!f || !f->_al5font) return 8;
    return al_get_font_line_height(f->_al5font);
}

int font_height(const FONT* f) { return text_height(f); }

// Internal helper: render text onto a BITMAP via Allegro 5 into a temp bitmap,
// then blit back (palette-approximate).  Phase 2 approach; Phase 4 will improve.
static void _textout_internal(BITMAP* bmp, const FONT* f, const char* str,
                               int x, int y, int fg, int bg, int align)
{
    if (!bmp || !f || !f->_al5font || !str) return;

    int tw = al_get_text_width(f->_al5font, str);
    int th = al_get_font_line_height(f->_al5font);
    if (tw <= 0 || th <= 0) return;

    // Adjust x for alignment
    if (align == 1) x -= tw / 2; // centre
    if (align == 2) x -= tw;     // right

    // Render background fill (palette colour bg; -1 = transparent)
    if (bg >= 0) rectfill(bmp, x, y, x + tw - 1, y + th - 1, bg);

    // Render each character as a coloured pixel block (Phase 2: bitmap font rendering
    // requires a proper glyph atlas; for now render via Allegro 5 into an RGBA temp
    // bitmap, then scan and approximate-map back to the nearest palette colour).
    if (_zc_palette_dirty) _al5_rebuild_color_cache();

    // Create a small temp ALLEGRO_BITMAP just wide enough for the text
    ALLEGRO_BITMAP* old_target = al_get_target_bitmap();
    al_set_new_bitmap_flags(ALLEGRO_MEMORY_BITMAP);
    ALLEGRO_BITMAP* tmp = al_create_bitmap(tw, th);
    if (!tmp) { al_set_target_bitmap(old_target); return; }

    al_set_target_bitmap(tmp);
    // Use transparent background
    al_clear_to_color(al_map_rgba(0, 0, 0, 0));
    al_draw_text(f->_al5font, al_map_rgb(255, 255, 255), 0, 0, 0, str);
    al_set_target_bitmap(old_target);

    // Lock the temp bitmap to read pixels
    ALLEGRO_LOCKED_REGION* region = al_lock_bitmap(
        tmp, ALLEGRO_PIXEL_FORMAT_ABGR_8888_LE, ALLEGRO_LOCK_READONLY);
    if (!region) { al_destroy_bitmap(tmp); return; }

    // Get the target foreground palette entry as (r,g,b)
    int fgr = getr(fg), fgg = getg(fg), fgb = getb(fg);

    for (int row = 0; row < th; row++) {
        int dy = y + row;
        if (dy < 0 || dy >= bmp->h) continue;
        if (bmp->clip && (dy < bmp->ct || dy >= bmp->cb)) continue;
        const uint32_t* src_row = reinterpret_cast<const uint32_t*>(
            reinterpret_cast<const uint8_t*>(region->data) + row * region->pitch);
        for (int col = 0; col < tw; col++) {
            int dx = x + col;
            if (dx < 0 || dx >= bmp->w) continue;
            if (bmp->clip && (dx < bmp->cl || dx >= bmp->cr)) continue;
            uint32_t pixel = src_row[col];
            uint8_t alpha = (pixel >> 24) & 0xFF;
            if (alpha < 64) continue; // transparent
            // Blend towards foreground colour
            uint8_t r = ((pixel      ) & 0xFF) * fgr / 255;
            uint8_t g = ((pixel >> 8 ) & 0xFF) * fgg / 255;
            uint8_t b = ((pixel >> 16) & 0xFF) * fgb / 255;
            bmp->line[dy][dx] = (uint8_t)_al5_bestfit_color(r, g, b);
        }
    }
    al_unlock_bitmap(tmp);
    al_destroy_bitmap(tmp);
}

void textout_ex(BITMAP* bmp, const FONT* f, const char* str, int x, int y, int fg, int bg)
{
    _textout_internal(bmp, f, str, x, y, fg, bg, 0);
}

void textout_centre_ex(BITMAP* bmp, const FONT* f, const char* str, int x, int y, int fg, int bg)
{
    _textout_internal(bmp, f, str, x, y, fg, bg, 1);
}

void textout_right_ex(BITMAP* bmp, const FONT* f, const char* str, int x, int y, int fg, int bg)
{
    _textout_internal(bmp, f, str, x, y, fg, bg, 2);
}

static void _textprintf_impl(BITMAP* bmp, const FONT* f, int x, int y,
                              int fg, int bg, int align, const char* fmt, va_list args)
{
    char buf[4096];
    vsnprintf(buf, sizeof(buf), fmt, args);
    _textout_internal(bmp, f, buf, x, y, fg, bg, align);
}

void textprintf_ex(BITMAP* bmp, const FONT* f, int x, int y, int fg, int bg,
                   const char* fmt, ...)
{
    va_list args; va_start(args, fmt);
    _textprintf_impl(bmp, f, x, y, fg, bg, 0, fmt, args);
    va_end(args);
}

void textprintf_centre_ex(BITMAP* bmp, const FONT* f, int x, int y, int fg, int bg,
                          const char* fmt, ...)
{
    va_list args; va_start(args, fmt);
    _textprintf_impl(bmp, f, x, y, fg, bg, 1, fmt, args);
    va_end(args);
}

void textprintf_right_ex(BITMAP* bmp, const FONT* f, int x, int y, int fg, int bg,
                         const char* fmt, ...)
{
    va_list args; va_start(args, fmt);
    _textprintf_impl(bmp, f, x, y, fg, bg, 2, fmt, args);
    va_end(args);
}

void textprintf_justify_ex(BITMAP* bmp, const FONT* f, int x1, int x2, int y,
                           int diff, int fg, int bg, const char* fmt, ...)
{
    (void)x2; (void)diff;
    va_list args; va_start(args, fmt);
    _textprintf_impl(bmp, f, x1, y, fg, bg, 0, fmt, args);
    va_end(args);
}

// ============================================================
// Keyboard
// ============================================================
int install_keyboard()
{
    if (!al_install_keyboard()) return -1;
    memset((void*)key, 0, sizeof(key));
    key_shifts = 0;
    if (_al5_event_queue)
        al_register_event_source(_al5_event_queue, al_get_keyboard_event_source());
    return 0;
}

void remove_keyboard()
{
    al_uninstall_keyboard();
}

int keypressed()
{
    return _keybuf_head != _keybuf_tail;
}

int readkey()
{
    while (_keybuf_head == _keybuf_tail) {
        // Pump events while waiting
        ALLEGRO_EVENT ev;
        if (_al5_event_queue && al_wait_for_event_timed(_al5_event_queue, &ev, 0.01f)) {
            if (ev.type == ALLEGRO_EVENT_KEY_CHAR) {
                int val = (ev.keyboard.unichar << 8) | (ev.keyboard.keycode & 0xFF);
                _keybuf[_keybuf_tail] = val;
                _keybuf_tail = (_keybuf_tail + 1) % KEYBUF_SIZE;
            }
        }
    }
    int val = _keybuf[_keybuf_head];
    _keybuf_head = (_keybuf_head + 1) % KEYBUF_SIZE;
    return val;
}

int ureadkey(int* scancode)
{
    int k = readkey();
    if (scancode) *scancode = k & 0xFF;
    return k >> 8;
}

void clear_keybuf()
{
    _keybuf_head = _keybuf_tail = 0;
}

// ============================================================
// Mouse
// ============================================================
int install_mouse()
{
    if (!al_install_mouse()) return -1;
    if (_al5_event_queue)
        al_register_event_source(_al5_event_queue, al_get_mouse_event_source());
    return 0;
}

void remove_mouse()
{
    al_uninstall_mouse();
}

void set_mouse_pos(int x, int y)
{
    if (_al5_display) al_set_mouse_xy(_al5_display, x, y);
    mouse_x = x; mouse_y = y;
}

void set_mouse_range(int x1, int y1, int x2, int y2)
{
    (void)x1; (void)y1; (void)x2; (void)y2; // handled by event clipping
}

void set_mouse_speed(int xspeed, int yspeed)
{
    (void)xspeed; (void)yspeed;
}

int get_mouse_mickeys(int* mickeyx, int* mickeyy)
{
    if (mickeyx) *mickeyx = 0;
    if (mickeyy) *mickeyy = 0;
    return 0;
}

// ============================================================
// Joystick
// ============================================================
int  install_joystick(int type) { (void)type; return 0; }
void remove_joystick()          {}

// ============================================================
// System
// ============================================================
int allegro_init()
{
    if (!al_init()) {
        snprintf(allegro_error, sizeof(allegro_error), "al_init() failed");
        return -1;
    }
    al_init_font_addon();
    al_init_ttf_addon();
    al_init_image_addon();
    al_init_primitives_addon();
    al_init_native_dialog_addon();
    if (!al_install_audio()) {
        // Audio is optional at init; zcsound handles it separately
    }
    al_init_acodec_addon();
    return 0;
}

void allegro_exit()
{
    if (_al5_display)     { al_destroy_display(_al5_display);     _al5_display = nullptr; }
    if (_al5_event_queue) { al_destroy_event_queue(_al5_event_queue); _al5_event_queue = nullptr; }
    if (_al5_timer)       { al_destroy_timer(_al5_timer);         _al5_timer = nullptr; }
    al_uninstall_system();
}

void set_window_title(const char* title)
{
    if (_al5_display) al_set_window_title(_al5_display, title);
}

static int _color_depth = 8;

int set_gfx_mode(int mode, int w, int h, int vw, int vh)
{
    (void)vw; (void)vh;

    // GFX_TEXT: switch to text mode (restore terminal) — no-op on macOS
    if (mode == GFX_TEXT) return 0;

    if (w <= 0 || h <= 0) return -1;

    // Destroy existing display if any
    if (_al5_display) {
        al_destroy_display(_al5_display);
        _al5_display = nullptr;
    }
    if (screen) { destroy_bitmap(screen); screen = nullptr; }

    // Create event queue
    if (!_al5_event_queue)
        _al5_event_queue = al_create_event_queue();

    // Set display flags
    bool fullscreen = (mode == GFX_AUTODETECT_FULLSCREEN || mode == GFX_QUARTZ_FULLSCREEN);
    al_set_new_display_flags(fullscreen ? ALLEGRO_FULLSCREEN : ALLEGRO_WINDOWED);
    al_set_new_display_option(ALLEGRO_COLOR_SIZE, 32, ALLEGRO_REQUIRE);

    _al5_display = al_create_display(w, h);
    if (!_al5_display) {
        snprintf(allegro_error, sizeof(allegro_error), "al_create_display(%d,%d) failed", w, h);
        return -1;
    }

    al_register_event_source(_al5_event_queue, al_get_display_event_source(_al5_display));

    // Create the screen bitmap (8-bit software buffer)
    screen = new BITMAP(w, h);
    _zc_screen_w = w;
    _zc_screen_h = h;

    return 0;
}

void set_color_depth(int depth)  { _color_depth = depth; }
int  get_color_depth()           { return _color_depth; }
int  desktop_color_depth()       { return 32; }

int get_desktop_resolution(int* w, int* h)
{
    ALLEGRO_MONITOR_INFO info;
    if (al_get_monitor_info(0, &info)) {
        if (w) *w = info.x2 - info.x1;
        if (h) *h = info.y2 - info.y1;
        return 0;
    }
    return -1;
}

void vsync()     { _al5_flip_display(); }
void rest(unsigned int ms) { al_rest(ms / 1000.0); }
void rest_callback(unsigned int ms, void (*cb)(void))
{
    rest(ms);
    if (cb) cb();
}

// ============================================================
// Event pump — drain Allegro 5 events → key[]/mouse_* globals
// ============================================================
void _al5_pump_events()
{
    if (!_al5_event_queue) return;

    ALLEGRO_EVENT ev;
    while (al_get_next_event(_al5_event_queue, &ev)) {
        switch (ev.type) {
        case ALLEGRO_EVENT_KEY_DOWN:
            if (ev.keyboard.keycode < ALLEGRO_KEY_MAX)
                key[ev.keyboard.keycode] = 1;
            break;
        case ALLEGRO_EVENT_KEY_UP:
            if (ev.keyboard.keycode < ALLEGRO_KEY_MAX)
                key[ev.keyboard.keycode] = 0;
            break;
        case ALLEGRO_EVENT_KEY_CHAR:
            key_shifts = ev.keyboard.modifiers;
            {
                int val = (ev.keyboard.unichar << 8) | (ev.keyboard.keycode & 0xFF);
                int next = (_keybuf_tail + 1) % KEYBUF_SIZE;
                if (next != _keybuf_head) {
                    _keybuf[_keybuf_tail] = val;
                    _keybuf_tail = next;
                }
            }
            break;
        case ALLEGRO_EVENT_MOUSE_AXES:
            mouse_x = ev.mouse.x;
            mouse_y = ev.mouse.y;
            mouse_z = ev.mouse.z;
            mouse_w = ev.mouse.w;
            break;
        case ALLEGRO_EVENT_MOUSE_BUTTON_DOWN:
            mouse_b |= (1 << (ev.mouse.button - 1));
            break;
        case ALLEGRO_EVENT_MOUSE_BUTTON_UP:
            mouse_b &= ~(1 << (ev.mouse.button - 1));
            break;
        case ALLEGRO_EVENT_DISPLAY_CLOSE:
            // Signal quit by setting a reserved key
            key[ALLEGRO_KEY_ESCAPE] = 1;
            break;
        default:
            break;
        }
    }

    // Fire timer callbacks
    int64_t now = _us_now();
    for (auto& cb : _timer_callbacks) {
        if (cb.proc && now >= cb.next_us) {
            cb.proc();
            cb.next_us = now + cb.period_us;
        }
    }
}

// ============================================================
// Display flip
// ============================================================
void _al5_flip_display()
{
    if (!_al5_display || !screen) return;
    screen->upload_to_al5();
    if (!screen->_al5bmp) return;

    ALLEGRO_BITMAP* old_target = al_get_target_bitmap();
    al_set_target_backbuffer(_al5_display);
    al_draw_bitmap(screen->_al5bmp, 0, 0, 0);
    al_flip_display();
    al_set_target_bitmap(old_target);
    retrace_count++;
}

// ============================================================
// Timer
// ============================================================
int install_timer()
{
    // Allegro 5 doesn't need explicit timer install; timers are per-object.
    return 0;
}

void remove_timer() {}

int install_int(void (*proc)(void), int msec)
{
    if (!proc) return -1;
    TimerCallback cb;
    cb.proc      = proc;
    cb.period_us = msec * 1000;
    cb.next_us   = _us_now() + cb.period_us;
    _timer_callbacks.push_back(cb);
    return 0;
}

int install_int_ex(void (*proc)(void), int speed)
{
    // speed is in Allegro 4 timer ticks (1193181 Hz)
    if (!proc || speed <= 0) return -1;
    TimerCallback cb;
    cb.proc      = proc;
    cb.period_us = (int)(speed * 1000000LL / 1193181LL);
    cb.next_us   = _us_now() + cb.period_us;
    _timer_callbacks.push_back(cb);
    return 0;
}

void remove_int(void (*proc)(void))
{
    _timer_callbacks.erase(
        std::remove_if(_timer_callbacks.begin(), _timer_callbacks.end(),
                       [proc](const TimerCallback& cb){ return cb.proc == proc; }),
        _timer_callbacks.end());
}

int install_param_int(void (*proc)(void*), void* param, int msec)
{
    (void)proc; (void)param; (void)msec; return 0; // stub
}
int install_param_int_ex(void (*proc)(void*), void* param, int speed)
{
    (void)proc; (void)param; (void)speed; return 0; // stub
}
void remove_param_int(void (*proc)(void*), void* param)
{
    (void)proc; (void)param;
}

unsigned long myclock()
{
    return (unsigned long)(_us_now() / 1000ULL);
}

long int myclock2()
{
    return (long int)(_us_now() / 1000ULL);
}

// ============================================================
// PACKFILE (thin wrapper around ALLEGRO_FILE)
// ============================================================
PACKFILE* pack_fopen(const char* filename, const char* mode)
{
    // Allegro 4 uses '!' prefix for password; strip it.
    if (filename && filename[0] == '!') {
        const char* p = strchr(filename + 1, '!');
        if (p) filename = p + 1;
    }
    ALLEGRO_FILE* f = al_fopen(filename, mode);
    if (!f) return nullptr;
    PACKFILE* pf = new PACKFILE();
    pf->_f = f;
    return pf;
}

void pack_fclose(PACKFILE* f)
{
    if (!f) return;
    if (f->_f) al_fclose(f->_f);
    delete f;
}

int pack_feof(PACKFILE* f)    { return (!f || !f->_f) ? 1 : al_feof(f->_f); }
int pack_ferror(PACKFILE* f)  { return (!f || !f->_f) ? 1 : al_ferror(f->_f); }

int pack_fseek(PACKFILE* f, int offset)
{
    if (!f || !f->_f) return -1;
    return al_fseek(f->_f, offset, ALLEGRO_SEEK_CUR) ? 0 : -1;
}

int pack_getc(PACKFILE* f)
{
    if (!f || !f->_f) return EOF;
    int c = al_fgetc(f->_f);
    return c;
}

int pack_putc(int c, PACKFILE* f)
{
    if (!f || !f->_f) return EOF;
    return al_fputc(c, f->_f);
}

int pack_fread(void* buf, int size, PACKFILE* f)
{
    if (!f || !f->_f || size <= 0) return 0;
    return (int)al_fread(f->_f, buf, size);
}

int pack_fwrite(const void* buf, int size, PACKFILE* f)
{
    if (!f || !f->_f || size <= 0) return 0;
    return (int)al_fwrite(f->_f, buf, size);
}

// Big-endian word / long
int pack_mgetw(PACKFILE* f)
{
    int b1 = pack_getc(f); int b2 = pack_getc(f);
    return (b1 << 8) | b2;
}
int pack_mputw(int w, PACKFILE* f)
{
    pack_putc((w >> 8) & 0xFF, f);
    return pack_putc(w & 0xFF, f);
}
int32_t pack_mgetl(PACKFILE* f)
{
    int b1=pack_getc(f), b2=pack_getc(f), b3=pack_getc(f), b4=pack_getc(f);
    return ((int32_t)b1 << 24) | ((int32_t)b2 << 16) | ((int32_t)b3 << 8) | b4;
}
int pack_mputl(int32_t l, PACKFILE* f)
{
    pack_putc((l >> 24) & 0xFF, f);
    pack_putc((l >> 16) & 0xFF, f);
    pack_putc((l >>  8) & 0xFF, f);
    return pack_putc(l & 0xFF, f);
}

// Little-endian word / long
int pack_igetw(PACKFILE* f)
{
    int b1 = pack_getc(f); int b2 = pack_getc(f);
    return b1 | (b2 << 8);
}
int pack_iputw(int w, PACKFILE* f)
{
    pack_putc(w & 0xFF, f);
    return pack_putc((w >> 8) & 0xFF, f);
}
int32_t pack_igetl(PACKFILE* f)
{
    int b1=pack_getc(f), b2=pack_getc(f), b3=pack_getc(f), b4=pack_getc(f);
    return (int32_t)(b1 | (b2 << 8) | (b3 << 16) | (b4 << 24));
}
int pack_iputl(int32_t l, PACKFILE* f)
{
    pack_putc( l        & 0xFF, f);
    pack_putc((l >>  8) & 0xFF, f);
    pack_putc((l >> 16) & 0xFF, f);
    return pack_putc((l >> 24) & 0xFF, f);
}

// ============================================================
// File system utilities
// ============================================================
int file_exists(const char* filename, int attrib, int* aret)
{
    (void)attrib;
    struct stat st;
    int exists = (stat(filename, &st) == 0);
    if (aret) *aret = exists ? 0 : 1;
    return exists;
}

int64_t file_size_ex(const char* filename)
{
    struct stat st;
    if (stat(filename, &st) == 0) return (int64_t)st.st_size;
    return 0;
}

char* get_filename(const char* path)
{
    const char* p = strrchr(path, '/');
    if (!p) p = strrchr(path, '\\');
    return (char*)(p ? p + 1 : path);
}

char* get_extension(const char* path)
{
    char* fn = get_filename(path);
    char* p = strrchr(fn, '.');
    return p ? p + 1 : (char*)"";
}

void put_backslash(char* filename)
{
    int len = strlen(filename);
    if (len > 0 && filename[len-1] != '/' && filename[len-1] != '\\') {
        filename[len]   = '/';
        filename[len+1] = '\0';
    }
}

int replace_filename(char* dest, const char* path, const char* filename, int size)
{
    const char* p = strrchr(path, '/');
    if (!p) p = strrchr(path, '\\');
    int dir_len = p ? (int)(p - path + 1) : 0;
    if (dir_len + (int)strlen(filename) >= size) return 0;
    memcpy(dest, path, dir_len);
    strcpy(dest + dir_len, filename);
    return 1;
}

int replace_extension(char* dest, const char* filename, const char* ext, int size)
{
    const char* dot = strrchr(get_filename(filename), '.');
    int base_len = dot ? (int)(dot - filename) : (int)strlen(filename);
    if (base_len + 1 + (int)strlen(ext) >= size) return 0;
    memcpy(dest, filename, base_len);
    dest[base_len] = '.';
    strcpy(dest + base_len + 1, ext);
    return 1;
}

void append_filename(char* dest, const char* path, const char* filename, int size)
{
    snprintf(dest, size, "%s%s", path, filename);
}

int get_filepath(const char* filename, char* buf, int size)
{
    const char* p = strrchr(filename, '/');
    if (!p) p = strrchr(filename, '\\');
    int len = p ? (int)(p - filename + 1) : 0;
    if (len >= size) return 0;
    memcpy(buf, filename, len);
    buf[len] = '\0';
    return 1;
}

bool is_relative_filename(const char* filename)
{
    return filename && filename[0] != '/' && !(filename[0] && filename[1] == ':');
}

// ============================================================
// Dialog stubs (Phase 2 — proper implementation in Phase 4)
// ============================================================
int do_dialog(DIALOG* dialog, int focus_obj)
{
    (void)dialog; (void)focus_obj;
    return D_O_K; // stub: immediately returns OK
}

int popup_dialog(DIALOG* dialog, int focus_obj)
{
    return do_dialog(dialog, focus_obj);
}

DIALOG_PLAYER* init_dialog(DIALOG* dialog, int focus_obj)
{
    (void)dialog; (void)focus_obj;
    return nullptr;
}

int update_dialog(DIALOG_PLAYER* player)
{
    (void)player;
    return D_CLOSE;
}

int shutdown_dialog(DIALOG_PLAYER* player)
{
    (void)player;
    return D_O_K;
}

int do_menu(MENU* menu, int x, int y)
{
    (void)menu; (void)x; (void)y;
    return -1;
}

MENU_PLAYER* init_menu(MENU_PLAYER* player, MENU* menu)
{
    (void)player; (void)menu;
    return nullptr;
}

int update_menu(MENU_PLAYER* player)
{
    (void)player;
    return -1;
}

int shutdown_menu(MENU_PLAYER* player)
{
    (void)player;
    return -1;
}

int alert(const char* s1, const char* s2, const char* s3,
          const char* b1, const char* b2, int c1, int c2)
{
    (void)s2; (void)s3; (void)b1; (void)b2; (void)c1; (void)c2;
    // Use Allegro 5 native dialog
    if (_al5_display) {
        ALLEGRO_DISPLAY* disp = _al5_display;
        char msg[1024];
        snprintf(msg, sizeof(msg), "%s", s1 ? s1 : "");
        ALLEGRO_TEXTLOG* log = al_open_native_text_log("Alert", 0);
        if (log) {
            al_append_native_text_log(log, "%s\n", msg);
            al_close_native_text_log(log);
        }
        (void)disp;
    } else {
        fprintf(stderr, "ALERT: %s\n", s1 ? s1 : "");
    }
    return 1; // first button
}

int alert3(const char* s1, const char* s2, const char* s3,
           const char* b1, const char* b2, const char* b3,
           int c1, int c2, int c3)
{
    return alert(s1, s2, s3, b1, b2, c1, c2);
    (void)b3; (void)c3;
}

int file_select_ex(const char* message, char* path, const char* ext,
                   int size, int w, int h)
{
    (void)message; (void)ext; (void)size; (void)w; (void)h;
    // Phase 2 stub — return empty path, indicating cancel
    if (path && size > 0) path[0] = '\0';
    return 0;
}

int broadcast_dialog_message(int msg, int c)
{
    (void)msg; (void)c;
    return D_O_K;
}

// ============================================================
// Configuration
// ============================================================
void set_config_file(const char* filename)
{
    if (_al5_config) { al_destroy_config(_al5_config); _al5_config = nullptr; }
    if (filename) {
        strncpy(_al5_config_path, filename, sizeof(_al5_config_path) - 1);
        _al5_config = al_load_config_file(filename);
    }
    if (!_al5_config) _al5_config = al_create_config();
}

void set_config_data(const char* data, int length)
{
    (void)length;
    if (_al5_config) { al_destroy_config(_al5_config); }
    _al5_config = al_create_config();
    // Phase 2: parsing raw INI data not implemented; create empty config
    (void)data;
}

void push_config_state()  {}
void pop_config_state()   {}

const char* get_config_string(const char* section, const char* name, const char* def)
{
    if (!_al5_config) return def;
    const char* val = al_get_config_value(_al5_config, section, name);
    return val ? val : def;
}

int get_config_int(const char* section, const char* name, int def)
{
    const char* s = get_config_string(section, name, nullptr);
    return s ? atoi(s) : def;
}

float get_config_float(const char* section, const char* name, float def)
{
    const char* s = get_config_string(section, name, nullptr);
    return s ? (float)atof(s) : def;
}

int get_config_hex(const char* section, const char* name, int def)
{
    const char* s = get_config_string(section, name, nullptr);
    return s ? (int)strtol(s, nullptr, 16) : def;
}

void set_config_string(const char* section, const char* name, const char* val)
{
    if (!_al5_config) _al5_config = al_create_config();
    al_set_config_value(_al5_config, section, name, val);
    if (_al5_config_path[0])
        al_save_config_file(_al5_config_path, _al5_config);
}

void set_config_int(const char* section, const char* name, int val)
{
    char buf[64]; snprintf(buf, sizeof(buf), "%d", val);
    set_config_string(section, name, buf);
}

void set_config_float(const char* section, const char* name, float val)
{
    char buf[64]; snprintf(buf, sizeof(buf), "%g", val);
    set_config_string(section, name, buf);
}

void set_config_hex(const char* section, const char* name, int val)
{
    char buf[64]; snprintf(buf, sizeof(buf), "%x", val);
    set_config_string(section, name, buf);
}

void override_config_string(const char* section, const char* name, const char* str)
{
    set_config_string(section, name, str);
}

// ============================================================
// Datafile stubs
// ============================================================
DATAFILE* load_datafile(const char* filename)
{
    // Phase 2 stub: datafile loading not implemented
    (void)filename;
    return nullptr;
}

void unload_datafile(DATAFILE* dat)
{
    (void)dat;
}

DATAFILE* find_datafile_object(DATAFILE* dat, const char* objectname)
{
    (void)objectname;
    return dat;
}

DATAFILE* load_datafile_object(const char* filename, const char* objectname)
{
    (void)filename; (void)objectname;
    return nullptr;
}

void unload_datafile_object(DATAFILE* dat)
{
    (void)dat;
}

// ============================================================
// Sound stubs
// ============================================================
int install_sound(int digi, int midi, const char* cfg)
{
    (void)digi; (void)midi; (void)cfg;
    return 0; // success stub; actual audio handled by zcsound
}

void remove_sound() {}

SAMPLE* load_sample(const char* filename)
{
    ALLEGRO_SAMPLE* s = al_load_sample(filename);
    if (!s) return nullptr;
    SAMPLE* samp = new SAMPLE();
    samp->_s = s;
    return samp;
}

void destroy_sample(SAMPLE* s)
{
    if (!s) return;
    if (s->_s) al_destroy_sample(s->_s);
    delete s;
}

void play_sample(SAMPLE* s, int vol, int pan, int freq, int loop)
{
    if (!s || !s->_s) return;
    float gain = vol / 255.0f;
    float spd  = freq / 1000.0f;
    (void)pan;
    al_play_sample(s->_s, gain, 0.0f, spd, loop ? ALLEGRO_PLAYMODE_LOOP : ALLEGRO_PLAYMODE_ONCE, nullptr);
}

void stop_sample(SAMPLE* s)
{
    if (!s || !s->_s) return;
    al_stop_samples();
}

void adjust_sample(SAMPLE* s, int vol, int pan, int freq, int loop)
{
    (void)s; (void)vol; (void)pan; (void)freq; (void)loop;
}

MIDI* load_midi(const char* filename)    { (void)filename; return nullptr; }
void  destroy_midi(MIDI* midi)           { delete midi; }
int   play_midi(MIDI* midi, int loop)    { (void)midi; (void)loop; return -1; }
void  stop_midi()                        {}
void  midi_pause()                       {}
void  midi_resume()                      {}
int   midi_pos = 0;

#endif // __APPLE__
