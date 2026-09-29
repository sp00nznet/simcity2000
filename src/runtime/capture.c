/*
 * capture.c - --headless and --record: run the game where nobody can see it,
 * and film it.
 *
 * --headless puts the game on a private Win32 desktop in this window station.
 * Its windows exist, get messages and paint, but are never shown on the
 * desktop the user is looking at -- which matters because this machine is
 * often driven over RDP from a phone, where a game window would take over the
 * screen (REPO_RULES section 13).
 *
 * --record out.mp4 composites every visible top-level window of that desktop
 * (the game frame, its dialogs) with PrintWindow at a fixed rate and pipes the
 * frames to ffmpeg as raw BGRA. It works headless or not.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

static HDESK  g_desk;
static HANDLE g_ffmpeg_in;
static int    g_w, g_h, g_fps;
static double g_seconds;

int headless_init(void) {
    g_desk = CreateDesktopA("sc2k-headless", NULL, NULL, 0, GENERIC_ALL, NULL);
    if (!g_desk) {
        fprintf(stderr, "[capture] headless desktop failed (error %lu)\n", GetLastError());
        return 0;
    }
    return 1;
}

/* Called on the game thread before it creates any window. */
static void input_game_thread(void);
void headless_attach(void) {
    input_game_thread();
    if (g_desk && !SetThreadDesktop(g_desk))
        fprintf(stderr, "[capture] SetThreadDesktop failed (error %lu)\n", GetLastError());
}

typedef struct { HWND list[64]; int n, all; } wins_t;

static BOOL CALLBACK collect(HWND h, LPARAM p) {
    wins_t *w = (wins_t *)p;
    DWORD pid;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId() && (IsWindowVisible(h) || w->all) && w->n < 64) w->list[w->n++] = h;
    return TRUE;
}

static DWORD WINAPI capture_thread(LPVOID unused) {
    (void)unused;
    if (g_desk) SetThreadDesktop(g_desk);
    HDC screen = GetDC(NULL);
    HDC canvas = CreateCompatibleDC(screen), win = CreateCompatibleDC(screen);
    BITMAPINFO bi = {0};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = g_w;
    bi.bmiHeader.biHeight = -g_h;          /* top-down, like ffmpeg wants */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void *pixels;
    HBITMAP frame = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &pixels, NULL, 0);
    SelectObject(canvas, frame);
    DWORD period = 1000 / g_fps, start = GetTickCount();
    for (unsigned n = 0;;) {                  /* n = frames written */
        PatBlt(canvas, 0, 0, g_w, g_h, BLACKNESS);
        wins_t w = {0};
        if (g_desk) EnumDesktopWindows(g_desk, collect, (LPARAM)&w);
        else EnumWindows(collect, (LPARAM)&w);
        static int trace = -1;
        if (trace < 0) trace = GetEnvironmentVariableA("SC2K_CAPTRACE", NULL, 0) != 0;
        if (trace && n % g_fps == 0) {
            wins_t a = {0};
            a.all = 1;
            if (g_desk) EnumDesktopWindows(g_desk, collect, (LPARAM)&a);
            else EnumWindows(collect, (LPARAM)&a);
            fprintf(stderr, "[capture] t=%us, %d windows:", n / g_fps, a.n);
            for (int i = 0; i < a.n; i++) {
                char cls[64], title[64];
                RECT r;
                GetClassNameA(a.list[i], cls, sizeof cls);
                GetWindowTextA(a.list[i], title, sizeof title);
                GetWindowRect(a.list[i], &r);
                fprintf(stderr, " [%s \"%s\" %ldx%ld%s]", cls, title, r.right - r.left, r.bottom - r.top,
                        IsWindowVisible(a.list[i]) ? "" : " hidden");
            }
            fprintf(stderr, "\n");
        }
        for (int i = w.n - 1; i >= 0; i--) {         /* bottom of the z-order first */
            RECT r;
            GetWindowRect(w.list[i], &r);
            int ww = r.right - r.left, wh = r.bottom - r.top;
            if (ww <= 0 || wh <= 0) continue;
            HBITMAP b = CreateCompatibleBitmap(screen, ww, wh);
            HGDIOBJ old = SelectObject(win, b);
            if (!PrintWindow(w.list[i], win, PW_RENDERFULLCONTENT)) PrintWindow(w.list[i], win, 0);
            BitBlt(canvas, r.left, r.top, ww, wh, win, 0, 0, SRCCOPY);
            SelectObject(win, old);
            DeleteObject(b);
        }
        GdiFlush();
        /* Keep the video's clock on the wall clock: when a capture takes
         * longer than a frame period, repeat the frame instead of drifting. */
        DWORD now = GetTickCount();
        unsigned due = (now - start) * g_fps / 1000 + 1, fail = 0;
        for (; n < due && !fail; n++) {
            DWORD wrote;
            fail = !WriteFile(g_ffmpeg_in, pixels, (DWORD)g_w * g_h * 4, &wrote, NULL);
        }
        if (fail || (g_seconds > 0 && now - start >= g_seconds * 1000)) break;
        DWORD next = start + n * period;
        now = GetTickCount();
        if ((int)(next - now) > 0) Sleep(next - now);
    }
    CloseHandle(g_ffmpeg_in);                /* ffmpeg finishes the file on EOF */
    Sleep(3000);
    fprintf(stderr, "[capture] recording ended\n");
    ExitProcess(0);
    return 0;
}

/* Start filming to `path` at `fps`; stop (and exit) after `seconds` if > 0. */
int record_start(const char *path, int fps, double seconds) {
    g_fps = fps > 0 ? fps : 10;
    g_seconds = seconds;
    g_w = GetSystemMetrics(SM_CXSCREEN) & ~1;
    g_h = GetSystemMetrics(SM_CYSCREEN) & ~1;
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, &sa, 1 << 20)) return 0;
    SetHandleInformation(wr, HANDLE_FLAG_INHERIT, 0);
    char cmd[2 * MAX_PATH];
    snprintf(cmd, sizeof cmd,
             "ffmpeg -loglevel error -y -f rawvideo -pix_fmt bgra -s %dx%d -r %d -i - "
             "-pix_fmt yuv420p -c:v libx264 \"%s\"", g_w, g_h, g_fps, path);
    STARTUPINFOA si = { sizeof si };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = rd;
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        fprintf(stderr, "[capture] cannot start ffmpeg (is it on PATH?)\n");
        return 0;
    }
    CloseHandle(rd);
    g_ffmpeg_in = wr;
    CreateThread(NULL, 0, capture_thread, NULL, 0, NULL);
    fprintf(stderr, "[capture] recording %dx%d @ %d fps to %s\n", g_w, g_h, g_fps, path);
    return 1;
}

static DWORD WINAPI timeout_thread(LPVOID p) {
    Sleep((DWORD)(uintptr_t)p);
    fprintf(stderr, "[capture] --seconds elapsed\n");
    ExitProcess(0);
    return 0;
}

/* Exit after `seconds` when nothing is recording (recording has its own stop). */
void exit_after(double seconds) {
    if (seconds > 0 && !g_ffmpeg_in)
        CreateThread(NULL, 0, timeout_thread, (LPVOID)(uintptr_t)(seconds * 1000), 0, NULL);
}

/* --input "T:click X,Y; T:key VK; ...": scripted input for headless runs.
 * At T seconds after start, post a left click to whatever window is at screen
 * point X,Y, or a key press (virtual-key code) to the game thread's focus
 * window. Posted, not injected: SendInput only reaches the input desktop,
 * which a headless run is never on. */
static DWORD g_game_tid;

static void input_game_thread(void) { g_game_tid = GetCurrentThreadId(); }

static DWORD WINAPI input_thread(LPVOID arg) {
    const char *s = arg;
    if (g_desk) SetThreadDesktop(g_desk);
    DWORD start = GetTickCount();
    for (;;) {
        double t;
        char verb[16];
        int a = 0, b = 0, used = 0;
        while (*s == ' ' || *s == ';') s++;
        if (!*s || sscanf(s, "%lf:%15[a-z] %d,%d%n", &t, verb, &a, &b, &used) < 3) break;
        s += used;
        while (*s && *s != ';') s++;
        DWORD due = start + (DWORD)(t * 1000), now = GetTickCount();
        if ((int)(due - now) > 0) Sleep(due - now);
        if (!strcmp(verb, "click")) {
            POINT pt = { a, b };
            HWND h = WindowFromPoint(pt);
            if (!h) { fprintf(stderr, "[input] nothing at %d,%d\n", a, b); continue; }
            ScreenToClient(h, &pt);
            LPARAM lp = MAKELPARAM(pt.x, pt.y);
            PostMessageA(h, WM_MOUSEMOVE, 0, lp);
            PostMessageA(h, WM_LBUTTONDOWN, MK_LBUTTON, lp);
            PostMessageA(h, WM_LBUTTONUP, 0, lp);
            fprintf(stderr, "[input] %.1fs click %d,%d -> %p\n", t, a, b, (void *)h);
        } else if (!strcmp(verb, "key")) {
            GUITHREADINFO gi = { sizeof gi };
            GetGUIThreadInfo(g_game_tid, &gi);
            HWND h = gi.hwndFocus ? gi.hwndFocus : gi.hwndActive;
            PostMessageA(h, WM_KEYDOWN, a, 1);
            PostMessageA(h, WM_KEYUP, a, 0xC0000001);
            fprintf(stderr, "[input] %.1fs key 0x%X -> %p\n", t, a, (void *)h);
        }
    }
    return 0;
}

void input_start(const char *script) {
    if (script) CreateThread(NULL, 0, input_thread, (LPVOID)script, 0, NULL);
}
