/*
 * qol.c - quality-of-life changes that need to sit between the game and
 * Windows: turbo speed.
 *
 * The game drives its simulation from one multimedia timer, a 200 ms
 * timeSetEvent whose callback (0x00402A72) advances the clock. Turbo shortens
 * that period by a factor, so African Swallow can go faster still. Changing
 * the factor re-arms the timer; the game keeps the ID it was first given,
 * because it kills the timer by that ID later.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <string.h>

typedef struct {
    UINT fake;                 /* the ID the game holds */
    UINT real;                 /* the live winmm timer */
    UINT delay, res, flags;
    LPTIMECALLBACK cb;
    DWORD_PTR user;
} game_timer_t;

static game_timer_t g_timers[8];
static int g_ntimers;
static int g_turbo = 1;
static CRITICAL_SECTION g_lock;
static LONG g_init;

static void init(void) {
    if (InterlockedCompareExchange(&g_init, 1, 0) == 0) InitializeCriticalSection(&g_lock);
    while (g_init != 1) Sleep(0);
}

static UINT arm(game_timer_t *t) {
    UINT d = t->delay / (UINT)g_turbo;
    return timeSetEvent(d ? d : 1, t->res / (UINT)g_turbo, t->cb, t->user, t->flags);
}

static MMRESULT WINAPI o_timeSetEvent(UINT delay, UINT res, LPTIMECALLBACK cb, DWORD_PTR user, UINT flags) {
    init();
    EnterCriticalSection(&g_lock);
    MMRESULT id = 0;
    if (g_ntimers < 8 && (flags & TIME_PERIODIC)) {
        game_timer_t *t = &g_timers[g_ntimers];
        *t = (game_timer_t){ 0, 0, delay, res, flags, cb, user };
        t->real = arm(t);
        t->fake = t->real;
        if (t->real) g_ntimers++;
        id = t->fake;
    } else {
        id = timeSetEvent(delay, res, cb, user, flags);   /* one-shots pass through */
    }
    LeaveCriticalSection(&g_lock);
    return id;
}

static MMRESULT WINAPI o_timeKillEvent(UINT id) {
    init();
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < g_ntimers; i++) {
        if (g_timers[i].fake == id) {
            MMRESULT r = timeKillEvent(g_timers[i].real);
            g_timers[i] = g_timers[--g_ntimers];
            LeaveCriticalSection(&g_lock);
            return r;
        }
    }
    LeaveCriticalSection(&g_lock);
    return timeKillEvent(id);
}

/* 1 = normal; 2, 4, 8 = that many times faster. */
void qol_set_turbo(int factor) {
    init();
    if (factor < 1) factor = 1;
    EnterCriticalSection(&g_lock);
    if (factor != g_turbo) {
        g_turbo = factor;
        for (int i = 0; i < g_ntimers; i++) {
            timeKillEvent(g_timers[i].real);
            g_timers[i].real = arm(&g_timers[i]);
        }
    }
    LeaveCriticalSection(&g_lock);
}

int qol_turbo(void) { return g_turbo; }

void *qol_override(const char *name) {
    if (!strcmp(name, "timeSetEvent")) return (void *)o_timeSetEvent;
    if (!strcmp(name, "timeKillEvent")) return (void *)o_timeKillEvent;
    return NULL;
}
