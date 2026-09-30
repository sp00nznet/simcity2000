/*
 * fx.c - day and night, seasons and weather, done the 1996 way: by changing
 * palette entries.
 *
 * The game draws the city in 256 colours, and palette.c owns the colour tables
 * of the map view's DIBs, so every entry can be recoloured on its way to the
 * screen with no change to the game's code. Working per entry rather than per
 * pixel is what makes night look like night: an entry that is a light (bright,
 * in the range the game already animates for traffic and signals, or a lit
 * window's yellow) keeps its brightness while everything else darkens.
 *
 * The clock is the game's own: the day counter at 0x004CAE04 (25 days a month,
 * 300 a year), so the sky stops when the game is paused and hurries at African
 * Swallow. Month and weather (0x004CB40C, an index into the game's own weather
 * names) drive the season and weather tints. See docs/effects.md.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fx.h"

#define GAME_DAYS    (*(volatile int32_t *)0x004CAE04)
#define GAME_WEATHER (*(volatile uint8_t *)0x004CB40C)

fx_settings_t g_fx = {
    .daynight = 1, .seasons = 1, .weather = 1,
    .cycle_days = 50,           /* two game months per day and night */
    .night_depth = 0.72f,
};

enum { W_COLD, W_CLEAR, W_HOT, W_FOGGY, W_CHILLY, W_OVERCAST, W_SNOW, W_RAIN,
       W_WINDY, W_BLIZZARD, W_HURRICANE, W_TORNADO };

void fx_init(void) {
    char v[64];
    if (GetEnvironmentVariableA("SC2K_FX", v, sizeof v)) {      /* e.g. "daynight,weather" or "none" */
        g_fx.daynight = strstr(v, "daynight") != NULL;
        g_fx.seasons = strstr(v, "season") != NULL;
        g_fx.weather = strstr(v, "weather") != NULL;
    }
    if (GetEnvironmentVariableA("SC2K_DAYCYCLE", v, sizeof v) && atoi(v) > 0) g_fx.cycle_days = atoi(v);
}

static float smooth(float a, float b, float x) {
    float t = (x - a) / (b - a);
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return t * t * (3 - 2 * t);
}

/* How dark it is, 0 (noon) .. 1 (midnight), for a phase in [0, 1). */
float fx_night(float phase) {
    float sun = 0.5f - 0.5f * cosf(6.2831853f * phase);   /* 0 at midnight, 1 at noon */
    return 1.0f - smooth(0.12f, 0.45f, sun);
}

void fx_state(fx_state_t *s) {
    int d = GAME_DAYS;
    if (d < 0) d = 0;
    s->month = (d / 25) % 12;
    s->weather = GAME_WEATHER;
    s->phase = g_fx.cycle_days > 0 ? (float)(d % g_fx.cycle_days) / g_fx.cycle_days : 0.5f;
    s->night = g_fx.daynight ? fx_night(s->phase) * g_fx.night_depth : 0.0f;
    s->evening = s->phase > 0.5f;
}

/* Is this entry a light: something that shines at night? */
int fx_is_light(int index, RGBQUAD c) {
    int lum = (c.rgbRed * 30 + c.rgbGreen * 59 + c.rgbBlue * 11) / 100;
    if (index >= 171 && index <= 219 && lum > 140) return 1;            /* traffic, signals */
    return c.rgbRed > 200 && c.rgbGreen > 170 && c.rgbBlue < 110;         /* lit-window yellow */
}

static unsigned char clamp8(float v) { return v < 0 ? 0 : v > 255 ? 255 : (unsigned char)(v + 0.5f); }

RGBQUAD fx_apply(int index, RGBQUAD in, const fx_state_t *s) {
    float r = in.rgbRed, g = in.rgbGreen, b = in.rgbBlue;
    int light = fx_is_light(index, in);
    float grey = 0.30f * r + 0.59f * g + 0.11f * b;

    if (g_fx.seasons) {
        int m = s->month;
        if (m == 11 || m <= 1) {                    /* winter: paler, cooler */
            r += (grey - r) * 0.25f; g += (grey - g) * 0.25f; b += (grey - b) * 0.25f + 6;
        } else if (m >= 8 && m <= 10 && g > r + 10 && g > b) {
            float k = (m == 8 ? 0.25f : m == 9 ? 0.55f : 0.40f);  /* autumn: greens turn */
            r += (g - r) * k; g *= 1.0f - 0.12f * k;
        } else if (m >= 5 && m <= 7) {              /* summer: warmer */
            r *= 1.04f; b *= 0.97f;
        }
    }

    if (g_fx.weather && !light) {
        switch (s->weather) {
        case W_COLD: case W_CHILLY: b += 8; r *= 0.97f; break;
        case W_HOT:       r *= 1.05f; g *= 1.02f; b *= 0.94f; break;
        case W_FOGGY:     r += (205 - r) * 0.35f; g += (205 - g) * 0.35f; b += (210 - b) * 0.35f; break;
        case W_OVERCAST:  r = (r + (grey - r) * 0.25f) * 0.9f; g = (g + (grey - g) * 0.25f) * 0.9f;
                          b = (b + (grey - b) * 0.25f) * 0.92f; break;
        case W_RAIN:      r *= 0.8f; g *= 0.83f; b *= 0.9f; break;
        case W_SNOW:      r += (245 - r) * 0.18f; g += (245 - g) * 0.18f; b += (250 - b) * 0.18f; break;
        case W_BLIZZARD:  r += (240 - r) * 0.35f; g += (240 - g) * 0.35f; b += (248 - b) * 0.35f; break;
        case W_HURRICANE: case W_TORNADO:
                          r *= 0.7f; g *= 0.75f; b *= 0.72f; break;
        }
    }

    float n = s->night;
    if (n > 0) {
        if (light) {
            float k = 1.0f + 0.15f * n;                 /* lights hold, even brighten */
            r *= k; g *= k; b *= k;
        } else {
            float keep = 1.0f - n;
            float dusk = s->evening ? 4.0f * n * (1.0f - n) : 0.0f;   /* orange only at dusk */
            r = r * keep + 30.0f * dusk;
            g = g * keep + 10.0f * dusk;
            b = b * keep + 34.0f * n;                   /* moonlight blue */
        }
    }
    RGBQUAD out = { clamp8(b), clamp8(g), clamp8(r), 0 };
    return out;
}

/* sc2k --selftest: the effect math, checked without a game running. Not
 * assert(): release builds define NDEBUG and would check nothing. */
#define CHECK(x) do { if (!(x)) { printf("fx: FAILED %s (line %d)\n", #x, __LINE__); fails++; } } while (0)

int fx_selftest(void) {
    int fails = 0;
    fx_settings_t saved = g_fx;
    g_fx.daynight = g_fx.seasons = g_fx.weather = 1;
    fx_state_t noon = { .month = 3, .weather = W_CLEAR, .phase = 0.5f, .night = 0 };
    fx_state_t night = { .month = 3, .weather = W_CLEAR, .phase = 0.0f, .night = 0.72f };
    RGBQUAD grass = { 40, 150, 60, 0 };                 /* BGR */
    RGBQUAD lamp = { 60, 220, 250, 0 };                 /* lit-window yellow */

    CHECK(fx_night(0.5f) < 0.01f && fx_night(0.0f) > 0.99f);         /* noon light, midnight dark */
    CHECK(fx_night(0.1f) > fx_night(0.2f) && fx_night(0.2f) > 0.0f); /* dawn brightens */
    RGBQUAD a = fx_apply(10, grass, &noon);
    CHECK(a.rgbRed == grass.rgbRed && a.rgbGreen == grass.rgbGreen && a.rgbBlue == grass.rgbBlue);
    RGBQUAD b = fx_apply(10, grass, &night);
    CHECK(b.rgbGreen < grass.rgbGreen / 2);                          /* night darkens the land */
    RGBQUAD c = fx_apply(40, lamp, &night);
    CHECK(fx_is_light(40, lamp) && c.rgbRed >= lamp.rgbRed);          /* ...and not the lights */
    fx_state_t fog = noon;
    fog.weather = W_FOGGY;
    RGBQUAD d = fx_apply(10, grass, &fog);
    CHECK(d.rgbRed > grass.rgbRed && d.rgbGreen > grass.rgbGreen);  /* fog lifts toward grey */
    fx_state_t autumn = noon;
    autumn.month = 9;
    RGBQUAD e = fx_apply(10, grass, &autumn);
    CHECK(e.rgbRed > grass.rgbRed + 30);                             /* October leaves */

    g_fx = saved;
    if (!fails) printf("fx: ok\n");
    return fails != 0;
}
