/*
 * mods.c - built-in mods and mod DLLs (sc2k_mod.h, docs/mods.md).
 *
 * Every mod is a name, a description and a tick. Built-ins live here; the
 * rest are DLLs in mods\ beside sc2k.exe, loaded at startup. A host thread
 * ticks the ones switched on about 20 times a second, so mods run headless as
 * well as in the frontend. Which are on comes from sc2k.ini ([mods]) and from
 * SC2K_MODS="Name,Name".
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "sc2k_mod.h"

void guest_lock(void);                     /* host.c */
void guest_unlock(void);

#define GAME_FUNDS   (*(volatile int32_t *)0x004CA444)
#define GAME_DAYS    (*(volatile int32_t *)0x004CAE04)
#define GAME_WEATHER (*(volatile uint8_t *)0x004CB40C)

typedef struct { sc2k_mod_info info; int on; } mod_t;

#define MAX_MODS 32
static mod_t g_mods[MAX_MODS];
static int g_nmods;

/* ---- the API handed to DLLs ---- */
static void *api_game(uint32_t va) { return (va >= 0x00400000u && va < 0x00600000u) ? (void *)(uintptr_t)va : NULL; }
static void api_log(const char *t) { fprintf(stderr, "[mod] %s\n", t); }
static int32_t api_days(void) { return GAME_DAYS; }
static int32_t api_funds(void) { return GAME_FUNDS; }
static void api_set_funds(int32_t v) { guest_lock(); GAME_FUNDS = v; guest_unlock(); }

static const sc2k_api g_api = { SC2K_MOD_API, api_game, guest_lock, guest_unlock, api_log, api_days, api_funds, api_set_funds };

/* ---- built-ins ---- */

/* Weather feeds the simulation (fire spreads in heat and wind), so this is a
 * gameplay change, not just a look. 1 is the game's own "Clear". */
static void always_sunny(void) {
    if (GAME_WEATHER != 1) { guest_lock(); GAME_WEATHER = 1; guest_unlock(); }
}

static void add(const char *name, const char *desc, void (*tick)(void)) {
    if (g_nmods == MAX_MODS) return;
    g_mods[g_nmods].info = (sc2k_mod_info){ name, desc, tick };
    g_mods[g_nmods++].on = 0;
}

static void load_dlls(void) {
    char dir[MAX_PATH], pat[MAX_PATH];
    GetModuleFileNameA(NULL, dir, sizeof dir);
    char *e = strrchr(dir, '\\');
    if (e) e[1] = 0;
    strcat(dir, "mods\\");
    snprintf(pat, sizeof pat, "%s*.dll", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        char path[MAX_PATH];
        snprintf(path, sizeof path, "%s%s", dir, fd.cFileName);
        HMODULE m = LoadLibraryA(path);
        sc2k_mod_register_fn reg = m ? (sc2k_mod_register_fn)GetProcAddress(m, "sc2k_mod_register") : NULL;
        sc2k_mod_info info = {0};
        if (reg && g_nmods < MAX_MODS && reg(&g_api, &info) && info.name && info.tick) {
            g_mods[g_nmods].info = info;
            g_mods[g_nmods++].on = 0;
            fprintf(stderr, "[mods] loaded %s from %s\n", info.name, fd.cFileName);
        } else {
            fprintf(stderr, "[mods] %s is not a mod (no sc2k_mod_register)\n", fd.cFileName);
            if (m) FreeLibrary(m);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static DWORD WINAPI mods_thread(LPVOID unused) {
    (void)unused;
    for (;;) {
        Sleep(50);
        for (int i = 0; i < g_nmods; i++)
            if (g_mods[i].on) g_mods[i].info.tick();
    }
    return 0;
}

void mods_start(void) {
    add("Always sunny", "Lock the weather to Clear. The simulation reads weather (fires spread in heat and wind).",
        always_sunny);
    load_dlls();
    char want[512];
    if (GetEnvironmentVariableA("SC2K_MODS", want, sizeof want))
        for (int i = 0; i < g_nmods; i++) g_mods[i].on = strstr(want, g_mods[i].info.name) != NULL;
    CreateThread(NULL, 0, mods_thread, NULL, 0, NULL);
}

int mods_count(void) { return g_nmods; }
const char *mods_name(int i) { return g_mods[i].info.name; }
const char *mods_description(int i) { return g_mods[i].info.description ? g_mods[i].info.description : ""; }
int mods_on(int i) { return g_mods[i].on; }
void mods_set(int i, int on) { g_mods[i].on = on; }
