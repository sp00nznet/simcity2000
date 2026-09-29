/*
 * palette.c - palette animation on a true-colour desktop.
 *
 * SimCity 2000 animates water, traffic and lights the 256-colour way: it draws
 * into 8-bit DIBs, blits them to its windows, and then calls AnimatePalette to
 * change entries 171..219 a few times a second. On an 8-bit display the screen
 * follows the palette with no redraw. On a 32-bit display the blitted pixels
 * are already colours, so nothing moves -- which is why the game warns "Some
 * animations in SimCity 2000 won't display correctly" and gives up.
 *
 * So the host plays the part of the palette hardware. Every blit from an 8-bit
 * DIB to a window is remembered (one entry per destination rectangle, newest
 * last). When the game animates the palette, the new entries are written into
 * each remembered source DIB's colour table and the remembered blits are
 * replayed, oldest first, which is what the monitor would have shown.
 *
 * Only on a desktop without a palette; on a real 8-bit display the hardware
 * does it and this stays out of the way.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>

#define MAX_BLITS 256

typedef struct {
    HWND  hwnd;
    HDC   src;
    RECT  dst;          /* device coordinates in the window's client area */
    int   sx, sy, sw, sh;
    DWORD rop;
} blit_t;

static blit_t g_blits[MAX_BLITS];
static int    g_nblits;
static CRITICAL_SECTION g_lock;
static int    g_mode = -1;   /* -1 unknown, 0 hardware palette (off), 1 emulate */

static int emulating(void) {
    if (g_mode < 0) {
        HDC dc = GetDC(NULL);
        g_mode = !(GetDeviceCaps(dc, RASTERCAPS) & RC_PALETTE);
        ReleaseDC(NULL, dc);
        InitializeCriticalSection(&g_lock);
    }
    return g_mode;
}

static int is_8bit_dib(HDC dc) {
    DIBSECTION ds;
    HGDIOBJ b = GetCurrentObject(dc, OBJ_BITMAP);
    return b && GetObjectA(b, sizeof ds, &ds) == sizeof ds && ds.dsBm.bmBitsPixel == 8;
}

static void remember(HDC dst, int x, int y, int w, int h, HDC src, int sx, int sy, int sw, int sh, DWORD rop) {
    if (!emulating()) return;
    HWND hwnd = WindowFromDC(dst);
    if (rop != SRCCOPY || !src || !hwnd || !is_8bit_dib(src)) return;
    POINT p[2] = { { x, y }, { x + w, y + h } };
    LPtoDP(dst, p, 2);
    blit_t b = { hwnd, src, { p[0].x, p[0].y, p[1].x, p[1].y }, sx, sy, sw, sh, rop };
    EnterCriticalSection(&g_lock);
    /* A blit to the same rectangle replaces the old one; everything else is
     * kept in order, since later blits can overlap earlier ones. */
    int j = 0;
    for (int i = 0; i < g_nblits; i++)
        if (!(g_blits[i].hwnd == hwnd && EqualRect(&g_blits[i].dst, &b.dst))) g_blits[j++] = g_blits[i];
    g_nblits = j;
    if (g_nblits == MAX_BLITS) {
        memmove(g_blits, g_blits + 1, (MAX_BLITS - 1) * sizeof g_blits[0]);
        g_nblits--;
    }
    g_blits[g_nblits++] = b;
    LeaveCriticalSection(&g_lock);
}

static BOOL WINAPI o_BitBlt(HDC d, int x, int y, int w, int h, HDC s, int sx, int sy, DWORD rop) {
    BOOL r = BitBlt(d, x, y, w, h, s, sx, sy, rop);
    if (r) remember(d, x, y, w, h, s, sx, sy, w, h, rop);
    return r;
}

static BOOL WINAPI o_StretchBlt(HDC d, int x, int y, int w, int h, HDC s, int sx, int sy, int sw, int sh, DWORD rop) {
    BOOL r = StretchBlt(d, x, y, w, h, s, sx, sy, sw, sh, rop);
    if (r) remember(d, x, y, w, h, s, sx, sy, sw, sh, rop);
    return r;
}

static BOOL WINAPI o_AnimatePalette(HPALETTE pal, UINT start, UINT n, const PALETTEENTRY *pe) {
    BOOL r = AnimatePalette(pal, start, n, pe);
    if (!r || !emulating() || start >= 256) return r;
    if (n > 256 - start) n = 256 - start;
    RGBQUAD q[256];
    for (UINT i = 0; i < n; i++) {
        q[i].rgbRed = pe[i].peRed;
        q[i].rgbGreen = pe[i].peGreen;
        q[i].rgbBlue = pe[i].peBlue;
        q[i].rgbReserved = 0;
    }
    EnterCriticalSection(&g_lock);
    int live = 0;
    for (int i = 0; i < g_nblits; i++) {
        blit_t *b = &g_blits[i];
        /* The window may be gone, or the game may have reused the DC for
         * something else: drop the entry rather than paint stale pixels. */
        if (!IsWindow(b->hwnd) || !is_8bit_dib(b->src)) continue;
        g_blits[live++] = *b;
        int seen = 0;
        for (int k = 0; k < live - 1; k++) seen |= g_blits[k].src == b->src;
        if (!seen) SetDIBColorTable(b->src, start, n, q);
    }
    g_nblits = live;
    for (int i = 0; i < g_nblits; i++) {
        blit_t *b = &g_blits[i];
        HDC dc = GetDC(b->hwnd);
        int w = b->dst.right - b->dst.left, h = b->dst.bottom - b->dst.top;
        if (w == b->sw && h == b->sh)
            BitBlt(dc, b->dst.left, b->dst.top, w, h, b->src, b->sx, b->sy, b->rop);
        else
            StretchBlt(dc, b->dst.left, b->dst.top, w, h, b->src, b->sx, b->sy, b->sw, b->sh, b->rop);
        ReleaseDC(b->hwnd, dc);
    }
    LeaveCriticalSection(&g_lock);
    return r;
}

/* The game picks its tiles by what the display can do: with no palette it
 * draws water and traffic in fixed colours, and nothing could ever animate.
 * So tell it the truth of 1996 -- a 256-colour palette display -- and let the
 * code above be the palette. Only display DCs; printers stay honest. */
static int WINAPI o_GetDeviceCaps(HDC dc, int what) {
    int v = GetDeviceCaps(dc, what);
    if (!emulating() || GetDeviceCaps(dc, TECHNOLOGY) != DT_RASDISPLAY) return v;
    switch (what) {
    case RASTERCAPS:  return v | RC_PALETTE;
    case BITSPIXEL:   return 8;
    case PLANES:      return 1;
    case NUMCOLORS:   return 20;
    case SIZEPALETTE: return 256;
    case NUMRESERVED: return 20;
    case COLORRES:    return 18;
    }
    return v;
}

void *palette_override(const char *name) {
    if (!lstrcmpA(name, "GetDeviceCaps")) return (void *)o_GetDeviceCaps;
    if (!lstrcmpA(name, "AnimatePalette")) return (void *)o_AnimatePalette;
    if (!lstrcmpA(name, "BitBlt")) return (void *)o_BitBlt;
    if (!lstrcmpA(name, "StretchBlt")) return (void *)o_StretchBlt;
    return NULL;
}
