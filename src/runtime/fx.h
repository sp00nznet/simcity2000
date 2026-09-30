/* fx.h - palette effects: day and night, seasons, weather. See fx.c. */
#ifndef SC2K_FX_H
#define SC2K_FX_H

#include <stdint.h>

typedef struct {
    int   daynight, seasons, weather;   /* switches */
    int   cycle_days;                    /* game days per day-and-night */
    float night_depth;                   /* 0 = no night, 1 = black */
} fx_settings_t;

typedef struct {
    int   month;        /* 0 = January */
    int   weather;      /* the game's weather index */
    float phase;        /* 0 = midnight, 0.5 = noon */
    float night;        /* 0 = day .. night_depth */
    int   evening;
} fx_state_t;

extern fx_settings_t g_fx;

void    fx_init(void);
void    fx_state(fx_state_t *s);
float   fx_night(float phase);
int     fx_is_light(int index, RGBQUAD c);
RGBQUAD fx_apply(int index, RGBQUAD in, const fx_state_t *s);
int     fx_selftest(void);

#endif
