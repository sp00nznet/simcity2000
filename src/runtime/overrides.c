/*
 * overrides.c - the Win32 imports that cannot simply pass through.
 *
 * Everything else in SIMCITY.EXE's import table is bound to the real API (see
 * host.c). These are the calls whose honest answer would describe the host
 * process instead of the game: which module is "the program", what its file
 * is called, what command line it was started with. Each override is an
 * ordinary WINAPI function; host.c puts its address in the IAT slot and the
 * native bridge calls it like any other API.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <commdlg.h>

#define GUEST_MODULE ((HMODULE)0x00400000)

extern char g_game_exe[MAX_PATH];

/* MFC takes its HINSTANCE, and with it every dialog, menu, bitmap and string,
 * from GetModuleHandleA(NULL). That has to be the game's image. */
static HMODULE WINAPI o_GetModuleHandleA(LPCSTR name) {
    return name ? GetModuleHandleA(name) : GUEST_MODULE;
}

static DWORD WINAPI o_GetModuleFileNameA(HMODULE m, LPSTR buf, DWORD n) {
    return GetModuleFileNameA(m ? m : GUEST_MODULE, buf, n);
}

/* The CRT builds argv from this, and argv[0] is how the game finds its own
 * folder. host.c builds it: the game's path plus the player's arguments. */
extern char g_guest_cmdline[];
static LPSTR WINAPI o_GetCommandLineA(void) { return g_guest_cmdline; }

/* The installer wrote the game's settings under HKLM\SOFTWARE\Maxis, which
 * needs administrator rights today. Keep them per-user instead; everything
 * else about the calls is the real registry. SC2K_REGTRACE=1 logs each one. */
static int regtrace = -1;
static HKEY reg_root(HKEY k) { return k == HKEY_LOCAL_MACHINE ? HKEY_CURRENT_USER : k; }
static void reg_log(const char *fn, HKEY k, const char *what, LONG r) {
    if (regtrace < 0) regtrace = GetEnvironmentVariableA("SC2K_REGTRACE", NULL, 0) != 0;
    if (regtrace) fprintf(stderr, "[reg] %s(%p, \"%s\") -> %ld\n", fn, (void *)k, what ? what : "", r);
}

static LONG WINAPI o_RegOpenKeyExA(HKEY k, LPCSTR sub, DWORD o, REGSAM sam, PHKEY out) {
    LONG r = RegOpenKeyExA(reg_root(k), sub, o, sam, out);
    reg_log("RegOpenKeyExA", k, sub, r);
    return r;
}

static LONG WINAPI o_RegCreateKeyExA(HKEY k, LPCSTR sub, DWORD res, LPSTR cls, DWORD o, REGSAM sam,
                                     LPSECURITY_ATTRIBUTES sa, PHKEY out, LPDWORD disp) {
    LONG r = RegCreateKeyExA(reg_root(k), sub, res, cls, o, sam, sa, out, disp);
    reg_log("RegCreateKeyExA", k, sub, r);
    return r;
}

static LONG WINAPI o_RegQueryValueExA(HKEY k, LPCSTR name, LPDWORD res, LPDWORD type, LPBYTE data, LPDWORD n) {
    LONG r = RegQueryValueExA(k, name, res, type, data, n);
    reg_log("RegQueryValueExA", k, name, r);
    return r;
}

static LONG WINAPI o_RegQueryValueA(HKEY k, LPCSTR sub, LPSTR data, PLONG n) {
    LONG r = RegQueryValueA(reg_root(k), sub, data, n);
    reg_log("RegQueryValueA", k, sub, r);
    return r;
}

static LONG WINAPI o_RegSetValueA(HKEY k, LPCSTR sub, DWORD type, LPCSTR data, DWORD n) {
    LONG r = RegSetValueA(reg_root(k), sub, type, data, n);
    reg_log("RegSetValueA", k, sub, r);
    return r;
}

static LONG WINAPI o_RegDeleteKeyA(HKEY k, LPCSTR sub) {
    LONG r = RegDeleteKeyA(reg_root(k), sub);
    reg_log("RegDeleteKeyA", k, sub, r);
    return r;
}

/* What the Windows 95 installer would have written, so the game does not stop
 * at "has not been properly registered". Only absent values are written, so a
 * player's own settings (and the game's own later writes) are kept. */
void registry_defaults(void) {
    char home[MAX_PATH];
    strcpy(home, g_game_exe);
    char *slash = strrchr(home, '\\');
    if (slash) *slash = 0;
    static const struct { const char *key, *name; const char *sub; DWORD dw; } v[] = {
        { "Registration", "Mayor Name",   "Mayor", 0 },
        { "Registration", "Company Name", "SimCity 2000 recomp", 0 },
        { "Localize",     "Language",     "USA", 0 },
        { "Paths", "Home",      "", 0 },
        { "Paths", "Cities",    "\\CITIES", 0 },
        { "Paths", "SaveGame",  "\\CITIES", 0 },
        { "Paths", "Scenarios", "\\SCENARIO", 0 },
        { "Paths", "Data",      "\\DATA", 0 },
        { "Paths", "Graphics",  "\\BITMAPS", 0 },
        { "Paths", "Music",     "\\SOUNDS", 0 },
        { "Paths", "TileSets",  "\\SCURKART", 0 },
        { "Paths", "Goodies",   "\\GOODIES", 0 },
        { "Version", "SimCity 2000", NULL, 0x100 },
        { "Version", "SCURK",        NULL, 0x100 },
        { "Options", "Disasters",  NULL, 1 },
        { "Options", "Music",      NULL, 1 },
        { "Options", "Sound",      NULL, 1 },
        { "Options", "AutoGoto",   NULL, 1 },
        { "Options", "AutoBudget", NULL, 0 },
        { "Options", "AutoSave",   NULL, 0 },
        { "Options", "Speed",      NULL, 1 },
    };
    for (size_t i = 0; i < sizeof v / sizeof v[0]; i++) {
        char path[128], buf[MAX_PATH + 32];
        HKEY k;
        snprintf(path, sizeof path, "Software\\Maxis\\SimCity 2000\\%s", v[i].key);
        if (RegCreateKeyExA(HKEY_CURRENT_USER, path, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &k, NULL))
            continue;
        if (RegQueryValueExA(k, v[i].name, NULL, NULL, NULL, NULL) == ERROR_FILE_NOT_FOUND) {
            if (v[i].sub) {
                const char *s = v[i].sub;
                if (!strcmp(v[i].key, "Paths")) { snprintf(buf, sizeof buf, "%s%s", home, s); s = buf; }
                RegSetValueExA(k, v[i].name, 0, REG_SZ, (const BYTE *)s, (DWORD)strlen(s) + 1);
            } else {
                RegSetValueExA(k, v[i].name, 0, REG_DWORD, (const BYTE *)&v[i].dw, 4);
            }
        }
        RegCloseKey(k);
    }
}

/* The File dialogs. MFC 4 finds its CFileDialog's window with a CBT hook: it
 * attaches the C++ object to the first window created on the thread after it
 * arms the hook, which on Windows 95 was the dialog. Today's common dialog
 * starts COM first, and the first window is COM's hidden
 * OleMainThreadWndClass (then a shell WorkerW); MFC attaches the dialog object
 * to that, and the first message to the real dialog dereferences a CWnd that
 * does not exist. The original SIMCITY.EXE crashes the same way on Windows 11.
 *
 * So for the length of the call a second CBT hook sits in front of MFC's and
 * hides every window creation but a dialog's (#32770) from it. Hooks run
 * newest first, and returning without CallNextHookEx lets the window be
 * created without MFC hearing of it. */
static HHOOK g_dlg_filter;

static LRESULT CALLBACK dialogs_only(int code, WPARAM w, LPARAM l) {
    if (code == HCBT_CREATEWND) {
        char cls[16];
        if (!GetClassNameA((HWND)w, cls, sizeof cls) || strcmp(cls, "#32770")) return 0;
    }
    return CallNextHookEx(g_dlg_filter, code, w, l);
}

/* The game's filter is "Simcity files (*.sc2) ", " *.sc2 " -- padded with
 * spaces, which Windows 95 ignored. Today the pattern matches no file, and its
 * extension becomes "sc2 ": a city typed as TESTCITY was saved as
 * "testcity.sc2 .sc2". Hand the dialog a copy with each string trimmed. */
static const char *trimmed_filter(const char *f, char *buf, size_t cap) {
    size_t n = 0;
    for (; f && *f; f += strlen(f) + 1) {
        const char *a = f, *z = f + strlen(f);
        while (*a == ' ') a++;
        while (z > a && z[-1] == ' ') z--;
        if (n + (size_t)(z - a) + 2 > cap) return NULL;
        memcpy(buf + n, a, z - a);
        n += z - a;
        buf[n++] = 0;
    }
    buf[n] = 0;
    return buf;
}

static BOOL file_dialog(BOOL (WINAPI *fn)(LPOPENFILENAMEA), LPOPENFILENAMEA o) {
    char buf[1024];
    LPCSTR filter = o->lpstrFilter;
    const char *t = trimmed_filter(filter, buf, sizeof buf);
    if (t) o->lpstrFilter = t;
    HHOOK outer = g_dlg_filter;       /* a dialog could open another */
    g_dlg_filter = SetWindowsHookExA(WH_CBT, dialogs_only, NULL, GetCurrentThreadId());
    BOOL r = fn(o);
    if (g_dlg_filter) UnhookWindowsHookEx(g_dlg_filter);
    g_dlg_filter = outer;
    o->lpstrFilter = filter;
    return r;
}

static BOOL WINAPI o_GetSaveFileNameA(LPOPENFILENAMEA o) { return file_dialog(GetSaveFileNameA, o); }
static BOOL WINAPI o_GetOpenFileNameA(LPOPENFILENAMEA o) { return file_dialog(GetOpenFileNameA, o); }

static const struct { const char *name; void *fn; } table[] = {
    { "GetSaveFileNameA",   (void *)o_GetSaveFileNameA },
    { "GetOpenFileNameA",   (void *)o_GetOpenFileNameA },
    { "RegOpenKeyExA",      (void *)o_RegOpenKeyExA },
    { "RegCreateKeyExA",    (void *)o_RegCreateKeyExA },
    { "RegQueryValueExA",   (void *)o_RegQueryValueExA },
    { "RegQueryValueA",     (void *)o_RegQueryValueA },
    { "RegSetValueA",       (void *)o_RegSetValueA },
    { "RegDeleteKeyA",      (void *)o_RegDeleteKeyA },
    { "GetModuleHandleA",   (void *)o_GetModuleHandleA },
    { "GetModuleFileNameA", (void *)o_GetModuleFileNameA },
    { "GetCommandLineA",    (void *)o_GetCommandLineA },
};

void *palette_override(const char *name);   /* palette.c */
void *input_override(const char *name);     /* input.c */
void *capture_override(const char *name);   /* capture.c */
void *frontend_override(const char *name);  /* frontend.cpp */

void *override_for(const char *dll, const char *name) {
    (void)dll;
    void *p = palette_override(name);
    if (!p) p = input_override(name);
    if (!p) p = capture_override(name);
    if (!p) p = frontend_override(name);
    if (p) return p;
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++)
        if (!strcmp(table[i].name, name)) return table[i].fn;
    return NULL;
}
