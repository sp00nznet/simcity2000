/*
 * frame.c - the game's picture, for the frontend (frontend.cpp).
 *
 * The game runs on the headless desktop. A thread on that desktop composites
 * what a player would see -- the main frame's client area, with every other
 * visible window of the game (dialogs, floating palettes, pop-up menus) laid
 * over it at its own position -- into a BGRA buffer, as often as it can. The
 * frontend takes the latest finished frame; the two never wait on each other
 * for more than a copy.
 *
 * The client area and not the whole window: the game's menu bar is shown by
 * the frontend itself (mirrored, see frontend.cpp), so the picture starts
 * below it.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

HDESK headless_desktop(void);              /* capture.c */
HWND  game_frame_window(void);

#define MAX_W 3840
#define MAX_H 2160

static uint32_t *g_buf[2];
static int g_w, g_h, g_ready = -1;         /* g_ready: index of the newest finished buffer */
static unsigned g_serial;
static POINT g_origin;                     /* screen position of the client area's corner */
static CRITICAL_SECTION g_lock;
static double g_capture_ms;
/* Plain WM_PRINT: two to four times faster than PW_RENDERFULLCONTENT here
 * (13-25 ms against 26-98 ms for the city view), and the game draws itself
 * with GDI, so nothing is lost. SC2K_CAPMODE=full switches back. */
static UINT g_pw_flags = 0;

typedef struct { HWND list[64]; int n; HWND frame; } wins_t;

static BOOL CALLBACK collect(HWND h, LPARAM p) {
    wins_t *w = (wins_t *)p;
    DWORD pid;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId() && h != w->frame && IsWindowVisible(h) && w->n < 64) w->list[w->n++] = h;
    return TRUE;
}

static DWORD WINAPI frame_thread(LPVOID unused) {
    (void)unused;
    HDESK desk = headless_desktop();
    if (desk) SetThreadDesktop(desk);
    HDC screen = GetDC(NULL);
    HDC canvas = CreateCompatibleDC(screen), win = CreateCompatibleDC(screen);
    HBITMAP bmp = NULL;
    void *pixels = NULL;
    int bw = 0, bh = 0;
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    for (;;) {
        HWND frame = game_frame_window();
        RECT cr;
        if (!frame || !IsWindow(frame) || !GetClientRect(frame, &cr) || cr.right <= 0 || cr.bottom <= 0) {
            Sleep(50);
            continue;
        }
        QueryPerformanceCounter(&t0);
        int w = cr.right < MAX_W ? cr.right : MAX_W, h = cr.bottom < MAX_H ? cr.bottom : MAX_H;
        if (w != bw || h != bh) {
            if (bmp) DeleteObject(bmp);
            BITMAPINFO bi = {0};
            bi.bmiHeader.biSize = sizeof bi.bmiHeader;
            bi.bmiHeader.biWidth = w;
            bi.bmiHeader.biHeight = -h;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &pixels, NULL, 0);
            SelectObject(canvas, bmp);
            bw = w;
            bh = h;
        }
        POINT o = { 0, 0 };
        ClientToScreen(frame, &o);
        PrintWindow(frame, canvas, PW_CLIENTONLY | g_pw_flags);

        wins_t list = { .frame = frame };
        EnumDesktopWindows(desk, collect, (LPARAM)&list);
        for (int i = list.n - 1; i >= 0; i--) {         /* bottom of the z-order first */
            RECT r;
            GetWindowRect(list.list[i], &r);
            int ww = r.right - r.left, wh = r.bottom - r.top;
            if (ww <= 0 || wh <= 0) continue;
            HBITMAP b = CreateCompatibleBitmap(screen, ww, wh);
            HGDIOBJ old = SelectObject(win, b);
            PrintWindow(list.list[i], win, g_pw_flags);
            BitBlt(canvas, r.left - o.x, r.top - o.y, ww, wh, win, 0, 0, SRCCOPY);
            SelectObject(win, old);
            DeleteObject(b);
        }
        GdiFlush();

        EnterCriticalSection(&g_lock);
        int next = g_ready == 0 ? 1 : 0;
        if (!g_buf[next]) g_buf[next] = malloc((size_t)MAX_W * MAX_H * 4);
        memcpy(g_buf[next], pixels, (size_t)w * h * 4);
        g_w = w;
        g_h = h;
        g_origin = o;
        g_ready = next;
        g_serial++;
        QueryPerformanceCounter(&t1);
        g_capture_ms = (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
        LeaveCriticalSection(&g_lock);
        Sleep(8);
    }
    return 0;
}

void frame_start(void) {
    char m[16];
    if (GetEnvironmentVariableA("SC2K_CAPMODE", m, sizeof m) && !strcmp(m, "full")) g_pw_flags = PW_RENDERFULLCONTENT;
    InitializeCriticalSection(&g_lock);
    CreateThread(NULL, 0, frame_thread, NULL, 0, NULL);
}

/* Copy the newest frame into dst (room for MAX_W x MAX_H). Returns its serial
 * number, or 0 if there is none yet; *w, *h get its size. */
unsigned frame_latest(uint32_t *dst, int *w, int *h, unsigned have) {
    unsigned serial = 0;
    EnterCriticalSection(&g_lock);
    if (g_ready >= 0 && g_serial != have) {
        memcpy(dst, g_buf[g_ready], (size_t)g_w * g_h * 4);
        *w = g_w;
        *h = g_h;
        serial = g_serial;
    }
    LeaveCriticalSection(&g_lock);
    return serial;
}

/* Where the picture's corner is on the game's (headless) screen. */
POINT frame_origin(void) {
    EnterCriticalSection(&g_lock);
    POINT o = g_origin;
    LeaveCriticalSection(&g_lock);
    return o;
}

double frame_capture_ms(void) { return g_capture_ms; }
