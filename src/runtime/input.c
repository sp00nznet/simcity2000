/*
 * input.c - --input "T:verb args; ...": scripted mouse and keyboard.
 *
 * Verbs, each at T seconds after start, in screen coordinates:
 *   click X,Y            left click on whatever window is at X,Y
 *   press X,Y,MS         hold the button MS ms (a toolbar button's variants menu)
 *   drag  X1,Y1,X2,Y2    button down, move, up (zones, roads, power lines)
 *   key   VK             key press (to the control clicked last, or the focus)
 *   type  TEXT           characters, the same way
 *   command ID           WM_COMMAND to the main frame (a menu item)
 *   wait  TITLE          until a game window titled TITLE (=TITLE: exactly)
 *                        is visible; later times count from then
 *
 * Messages are posted, not injected: SendInput only reaches the input desktop,
 * which a headless run is never on. Posting alone is not enough, though. The
 * game reads the live cursor and button while a tool is dragged
 * (GetCursorPos, GetKeyState, GetAsyncKeyState, GetMessagePos), and the real
 * cursor never moves in a headless run -- zones came out one tile wide. So
 * while a script runs, those four report a synthetic cursor that the script
 * moves in step with the messages it posts.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

HDESK headless_desktop(void);     /* capture.c */

static int   g_scripted;
static int   g_live;             /* the frontend is feeding real input */
static POINT g_cur;
static int   g_ldown, g_rdown;
static DWORD g_game_tid;

void input_game_thread(void) { g_game_tid = GetCurrentThreadId(); }
void input_set_game_thread(DWORD tid) { g_game_tid = tid; }   /* --native */

/* Move the synthetic cursor to client point `pt` of `h`, then post `msg`. */
static void mouse(HWND h, UINT msg, POINT pt) {
    POINT s = pt;
    ClientToScreen(h, &s);
    g_cur = s;
    if (msg == WM_LBUTTONDOWN) g_ldown = 1;
    if (msg == WM_LBUTTONUP) g_ldown = 0;
    PostMessageA(h, msg, g_ldown ? MK_LBUTTON : 0, MAKELPARAM(pt.x, pt.y));
}

/* The window at a screen point, among this game's windows. WindowFromPoint
 * is not used: on a headless desktop it kept answering with the main frame
 * while a modal File dialog sat on top of it. Top-level windows are walked
 * in z-order, then children are descended into. */
typedef struct { POINT pt; DWORD pid; HWND hit; } hit_t;

static BOOL CALLBACK top_hit(HWND h, LPARAM p) {
    hit_t *t = (hit_t *)p;
    DWORD pid;
    RECT r;
    GetWindowThreadProcessId(h, &pid);
    if (pid != t->pid || !IsWindowVisible(h) || !GetWindowRect(h, &r) || !PtInRect(&r, t->pt)) return TRUE;
    t->hit = h;
    return FALSE;
}

static DWORD game_pid(void) {
    DWORD pid = GetCurrentProcessId();
    if (g_game_tid) {
        HANDLE th = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, g_game_tid);
        if (th) { pid = GetProcessIdOfThread(th); CloseHandle(th); }
    }
    return pid;
}

static HWND window_at(POINT pt) {
    hit_t t = { pt, game_pid(), NULL };
    HDESK desk = headless_desktop();
    if (desk) EnumDesktopWindows(desk, top_hit, (LPARAM)&t);
    else EnumWindows(top_hit, (LPARAM)&t);
    HWND h = t.hit;
    while (h) {
        POINT c = pt;
        ScreenToClient(h, &c);
        HWND k = ChildWindowFromPointEx(h, c, CWP_SKIPINVISIBLE | CWP_SKIPDISABLED | CWP_SKIPTRANSPARENT);
        if (!k || k == h) break;
        h = k;
    }
    return h;
}

typedef struct { const char *want; DWORD pid; HWND hit; } title_t;

static BOOL CALLBACK top_titled(HWND h, LPARAM p) {
    title_t *t = (title_t *)p;
    DWORD pid;
    char title[256];
    GetWindowThreadProcessId(h, &pid);
    if (pid != t->pid || !IsWindowVisible(h) || !GetWindowTextA(h, title, sizeof title)) return TRUE;
    if (t->want[0] == '=' ? !strcmp(title, t->want + 1) : strstr(title, t->want) != NULL) {
        t->hit = h;
        return FALSE;
    }
    return TRUE;
}

/* The File dialogs were not top-level windows of the game on a headless
 * desktop, so the children of every game window are searched too. */
static BOOL CALLBACK any_titled(HWND h, LPARAM p) {
    title_t *t = (title_t *)p;
    DWORD pid;
    GetWindowThreadProcessId(h, &pid);
    if (pid != t->pid) return TRUE;
    if (!top_titled(h, p)) return FALSE;
    EnumChildWindows(h, top_titled, p);
    return t->hit == NULL;
}

static HWND titled(const char *want) {
    title_t t = { want, game_pid(), NULL };
    HDESK desk = headless_desktop();
    if (desk) EnumDesktopWindows(desk, any_titled, (LPARAM)&t);
    else EnumWindows(any_titled, (LPARAM)&t);
    return t.hit;
}

static HWND at(int x, int y, POINT *client) {
    POINT pt = { x, y };
    HWND h = window_at(pt);
    if (!h) { fprintf(stderr, "[input] nothing at %d,%d\n", x, y); return NULL; }
    ScreenToClient(h, &pt);
    *client = pt;
    return h;
}

static DWORD WINAPI input_thread(LPVOID arg) {
    const char *s = arg;
    HDESK desk = headless_desktop();
    if (desk) SetThreadDesktop(desk);
    DWORD start = GetTickCount();
    HWND last_click = NULL;
    double last_click_t = 0;
    for (;;) {
        double t;
        char verb[16];
        char args[256];
        int a = 0, b = 0, c = 0, d = 0, used = 0;
        while (*s == ' ' || *s == ';') s++;
        if (!*s || sscanf(s, "%lf:%15[a-z]%n", &t, verb, &used) < 2) break;
        s += used;
        while (*s == ' ') s++;
        int n = 0;
        while (s[n] && s[n] != ';' && n < (int)sizeof args - 1) { args[n] = s[n]; n++; }
        args[n] = 0;
        s += n;
        sscanf(args, "%i,%i,%i,%i", &a, &b, &c, &d);
        DWORD due = start + (DWORD)(t * 1000), now = GetTickCount();
        if ((int)(due - now) > 0) Sleep(due - now);
        POINT p;
        HWND h;
        GUITHREADINFO gi = { sizeof gi };
        GetGUIThreadInfo(g_game_tid, &gi);
        /* Keys and text go to the control clicked last, if that was recent --
         * a posted click does not always move the focus (a File dialog's name
         * box kept losing the text) -- otherwise to the focus window. */
        HWND focus = gi.hwndFocus ? gi.hwndFocus : gi.hwndActive;
        if (last_click && IsWindow(last_click) && t - last_click_t < 10) focus = last_click;
        if (!strcmp(verb, "wait")) {
            /* wait TITLE (=TITLE for exact): until a visible window of the
             * game has that title, up to 60 s; later steps then count from
             * here, so a dialog that opens late does not break the script. */
            DWORD t0 = GetTickCount();
            while (!(h = titled(args)) && GetTickCount() - t0 < 60000) Sleep(100);
            start += GetTickCount() - (start + (DWORD)(t * 1000));
            fprintf(stderr, "[input] %.1fs wait \"%s\" -> %s\n", t, args, h ? "found" : "TIMED OUT");
            continue;
        } else if (!strcmp(verb, "key")) {
            h = focus;
            PostMessageA(h, WM_KEYDOWN, a, 1);
            PostMessageA(h, WM_KEYUP, a, 0xC0000001);
        } else if (!strcmp(verb, "type")) {
            /* type TEXT: characters, e.g. a file name */
            h = focus;
            char cls[16];
            if (GetClassNameA(h, cls, sizeof cls) && !_stricmp(cls, "Edit"))
                PostMessageA(h, EM_SETSEL, 0, -1);    /* typing replaces the field */
            for (const char *q = args; *q; q++) PostMessageA(h, WM_CHAR, (unsigned char)*q, 1);
        } else if (!strcmp(verb, "command")) {
            /* command ID: a menu command to the game's main frame */
            h = gi.hwndActive;
            while (h && GetParent(h)) h = GetParent(h);
            while (h && GetWindow(h, GW_OWNER)) h = GetWindow(h, GW_OWNER);
            PostMessageA(h, WM_COMMAND, a, 0);
        } else if (!(h = at(a, b, &p))) {
            continue;
        } else if (!strcmp(verb, "click") || !strcmp(verb, "press")) {
            mouse(h, WM_MOUSEMOVE, p);
            mouse(h, WM_LBUTTONDOWN, p);
            Sleep(!strcmp(verb, "press") ? (c > 0 ? c : 1000) : 50);
            mouse(h, WM_LBUTTONUP, p);
            last_click = h;
            last_click_t = t;
        } else if (!strcmp(verb, "drag")) {
            POINT q = { c, d };
            ScreenToClient(h, &q);
            mouse(h, WM_MOUSEMOVE, p);
            mouse(h, WM_LBUTTONDOWN, p);
            for (int k = 1; k <= 16; k++) {
                Sleep(50);
                POINT m = { p.x + (q.x - p.x) * k / 16, p.y + (q.y - p.y) * k / 16 };
                mouse(h, WM_MOUSEMOVE, m);
            }
            Sleep(200);
            mouse(h, WM_LBUTTONUP, q);
        } else {
            fprintf(stderr, "[input] unknown verb %s\n", verb);
            continue;
        }
        fprintf(stderr, "[input] %.1fs %s %d,%d%s -> %p\n", t, verb, a, b,
                !strcmp(verb, "drag") ? " (drag)" : "", (void *)h);
    }
    if (!g_live) g_scripted = 0;     /* the real cursor again, once the script is done */
    return 0;
}

void input_start(const char *script) {
    if (!script) return;
    g_scripted = 1;
    CreateThread(NULL, 0, input_thread, (LPVOID)script, 0, NULL);
}

static BOOL WINAPI o_GetCursorPos(LPPOINT p) {
    if (!g_scripted) return GetCursorPos(p);
    *p = g_cur;
    return TRUE;
}

static DWORD WINAPI o_GetMessagePos(void) {
    return g_scripted ? (DWORD)MAKELONG(g_cur.x, g_cur.y) : GetMessagePos();
}

/* Buttons come from the synthetic state. Keys too, in a script; with the
 * frontend the player is really pressing them, but on another thread's queue,
 * so the game thread's GetKeyState would never see Shift: ask the hardware. */
static SHORT WINAPI o_GetKeyState(int vk) {
    if (g_scripted && vk == VK_LBUTTON) return g_ldown ? (SHORT)0x8000 : 0;
    if (g_scripted && vk == VK_RBUTTON) return g_rdown ? (SHORT)0x8000 : 0;
    if (g_live) return (SHORT)(GetAsyncKeyState(vk) & 0x8000);
    return GetKeyState(vk);
}

static SHORT WINAPI o_GetAsyncKeyState(int vk) {
    if (g_scripted && vk == VK_LBUTTON) return g_ldown ? (SHORT)0x8000 : 0;
    if (g_scripted && vk == VK_RBUTTON) return g_rdown ? (SHORT)0x8000 : 0;
    return GetAsyncKeyState(vk);
}

void *input_override(const char *name) {
    if (!strcmp(name, "GetCursorPos")) return (void *)o_GetCursorPos;
    if (!strcmp(name, "GetMessagePos")) return (void *)o_GetMessagePos;
    if (!strcmp(name, "GetKeyState")) return (void *)o_GetKeyState;
    if (!strcmp(name, "GetAsyncKeyState")) return (void *)o_GetAsyncKeyState;
    return NULL;
}

/* ---- live input, from the frontend ----
 *
 * The frontend's window is on the user's desktop and the game on the headless
 * one, so its mouse and keys are queued here and delivered by a worker thread
 * that lives on the game's desktop. Coordinates arrive in the picture's space
 * (frame.c: the main frame's client area). A button press picks its window
 * by hit test and keeps it until release, the way mouse capture would. */
POINT frame_origin(void);                  /* frame.c */
HWND  game_frame_window(void);             /* capture.c */

typedef struct { int kind; UINT msg; WPARAM w; LPARAM l; int x, y; } live_ev_t;
enum { EV_MOUSE, EV_KEY, EV_COMMAND, EV_INITMENU };

#define LIVE_Q 1024
static live_ev_t g_q[LIVE_Q];
static volatile LONG g_qhead, g_qtail;
static HANDLE g_qevent;
static CRITICAL_SECTION g_qlock;

static void live_push(live_ev_t e) {
    EnterCriticalSection(&g_qlock);
    if (g_qtail - g_qhead < LIVE_Q) g_q[g_qtail++ % LIVE_Q] = e;
    LeaveCriticalSection(&g_qlock);
    SetEvent(g_qevent);
}

static HWND game_focus(void) {
    GUITHREADINFO gi = { sizeof gi };
    GetGUIThreadInfo(g_game_tid, &gi);
    return gi.hwndFocus ? gi.hwndFocus : gi.hwndActive ? gi.hwndActive : game_frame_window();
}

static void live_mouse(live_ev_t *e) {
    static HWND capture;
    POINT o = frame_origin();
    POINT scr = { o.x + e->x, o.y + e->y };
    HWND h;
    if (e->msg == WM_MOUSEWHEEL) {
        h = window_at(scr);
        if (h) PostMessageA(h, WM_MOUSEWHEEL, e->w, MAKELPARAM(scr.x, scr.y));   /* wheel is in screen space */
        return;
    }
    int down = e->msg == WM_LBUTTONDOWN || e->msg == WM_RBUTTONDOWN;
    h = (capture && IsWindow(capture) && !down) ? capture : window_at(scr);
    if (!h) return;
    if (down) capture = h;
    if (e->msg == WM_LBUTTONDOWN) g_ldown = 1;
    if (e->msg == WM_LBUTTONUP) g_ldown = 0;
    if (e->msg == WM_RBUTTONDOWN) g_rdown = 1;
    if (e->msg == WM_RBUTTONUP) g_rdown = 0;
    if (!g_ldown && !g_rdown) capture = NULL;
    g_cur = scr;
    POINT c = scr;
    ScreenToClient(h, &c);
    PostMessageA(h, e->msg, e->w, MAKELPARAM(c.x, c.y));
}

static DWORD WINAPI live_thread(LPVOID unused) {
    (void)unused;
    HDESK desk = headless_desktop();
    if (desk) SetThreadDesktop(desk);
    for (;;) {
        WaitForSingleObject(g_qevent, INFINITE);
        for (;;) {
            live_ev_t e;
            EnterCriticalSection(&g_qlock);
            int have = g_qhead != g_qtail;
            if (have) e = g_q[g_qhead++ % LIVE_Q];
            LeaveCriticalSection(&g_qlock);
            if (!have) break;
            switch (e.kind) {
            case EV_MOUSE: live_mouse(&e); break;
            case EV_KEY: PostMessageA(game_focus(), e.msg, e.w, e.l); break;
            case EV_COMMAND: PostMessageA(game_frame_window(), WM_COMMAND, e.w, 0); break;
            case EV_INITMENU:
                /* MFC sets check marks and greying in WM_INITMENUPOPUP; send
                 * it so the frontend's copy of the menu reads current state. */
                SendMessageTimeoutA(game_frame_window(), WM_INITMENUPOPUP, e.w, e.l, SMTO_ABORTIFHUNG, 200, NULL);
                break;
            }
        }
    }
    return 0;
}

void input_live_start(void) {
    InitializeCriticalSection(&g_qlock);
    g_qevent = CreateEventA(NULL, FALSE, FALSE, NULL);
    g_scripted = 1;
    g_live = 1;
    CreateThread(NULL, 0, live_thread, NULL, 0, NULL);
}

void input_live_mouse(UINT msg, WPARAM keys, int x, int y) { live_ev_t e = { EV_MOUSE, msg, keys, 0, x, y }; live_push(e); }
void input_live_key(UINT msg, WPARAM w, LPARAM l)          { live_ev_t e = { EV_KEY, msg, w, l, 0, 0 }; live_push(e); }
void input_live_command(UINT id)                           { live_ev_t e = { EV_COMMAND, 0, id, 0, 0, 0 }; live_push(e); }
void input_live_initmenu(HMENU sub, int index)             { live_ev_t e = { EV_INITMENU, 0, (WPARAM)sub, index, 0, 0 }; live_push(e); }
