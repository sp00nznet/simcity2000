/*
 * qol.c - quality-of-life changes that need to sit between the game and
 * Windows: game speed.
 *
 * The game drives its simulation from one multimedia timer, a 200 ms
 * timeSetEvent whose callback (0x00402A72) advances the clock -- a game day
 * per tick, so a year a minute on any modern PC. The speed scales that
 * period, slower or faster than the game's own speeds. Changing it re-arms
 * the timer; the game keeps the ID it was first given,
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
static int g_pct = 100;      /* speed, percent of normal */
static CRITICAL_SECTION g_lock;
static LONG g_init;

static void init(void) {
    if (InterlockedCompareExchange(&g_init, 1, 0) == 0) InitializeCriticalSection(&g_lock);
    while (g_init != 1) Sleep(0);
}

static UINT arm(game_timer_t *t) {
    UINT d = t->delay * 100 / (UINT)g_pct;
    return timeSetEvent(d ? d : 1, t->res * 100 / (UINT)g_pct, t->cb, t->user, t->flags);
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

/* percent of normal: 25 = a quarter speed, 400 = four times faster */
void qol_set_speed(int pct) {
    init();
    if (pct < 5) pct = 5;
    if (pct > 1000) pct = 1000;
    EnterCriticalSection(&g_lock);
    if (pct != g_pct) {
        g_pct = pct;
        for (int i = 0; i < g_ntimers; i++) {
            timeKillEvent(g_timers[i].real);
            g_timers[i].real = arm(&g_timers[i]);
        }
    }
    LeaveCriticalSection(&g_lock);
}


/* African Swallow (speed 5) skips the timer: the game's idle handler runs a
 * day on every pass, as fast as the CPU goes. A 1996 PC managed a few days a
 * second; now a year of an empty city is gone in a fifth of a second and the
 * budget and newspaper arrive back to back. So it gets a quota instead: two
 * days a timer tick, twice Cheetah, scaled by the speed like everything else.
 * ponytail: credit capped at two days, so a pause or a dialog banks nothing. */
int qol_swallow_day(void) {
    static double credit;
    static DWORD last;
    DWORD now = timeGetTime();
    if (last) credit += (now - last) * 0.01 * g_pct / 100;
    last = now;
    if (credit > 2) credit = 2;
    if (credit < 1) return 0;
    credit -= 1;
    return 1;
}

void *qol_override(const char *name) {
    if (!strcmp(name, "timeSetEvent")) return (void *)o_timeSetEvent;
    if (!strcmp(name, "timeKillEvent")) return (void *)o_timeKillEvent;
    return NULL;
}
