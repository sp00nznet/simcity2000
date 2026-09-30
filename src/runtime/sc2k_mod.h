/*
 * sc2k_mod.h - the mod API. A mod is a DLL in the mods\ folder beside
 * sc2k.exe that exports:
 *
 *     int sc2k_mod_register(const sc2k_api *api, sc2k_mod_info *info);
 *
 * It fills in `info` (name, description, tick) and returns 1 to load. The host
 * calls `tick` about 20 times a second while the mod is switched on (Mods
 * menu, or SC2K_MODS). Everything in the API is safe to call from tick.
 *
 * Game memory is the recompiled game's own: the addresses are those of the
 * original SIMCITY.EXE (docs/effects.md lists the ones known so far). Write
 * only between api->lock() and api->unlock(), so the game never sees a value
 * half-changed. See docs/mods.md.
 */
#ifndef SC2K_MOD_H
#define SC2K_MOD_H

#include <stdint.h>

#define SC2K_MOD_API 1

typedef struct {
    int      version;                     /* SC2K_MOD_API */
    void    *(*game)(uint32_t va);        /* pointer to game memory at a SIMCITY.EXE address */
    void     (*lock)(void);               /* hold the game still ... */
    void     (*unlock)(void);             /* ... and let it go */
    void     (*log)(const char *text);    /* to the host's log */
    int32_t  (*days)(void);               /* the game clock: 25 days a month, 300 a year */
    int32_t  (*funds)(void);
    void     (*set_funds)(int32_t v);     /* takes the lock itself */
} sc2k_api;

typedef struct {
    const char *name;
    const char *description;
    void (*tick)(void);
} sc2k_mod_info;

typedef int (*sc2k_mod_register_fn)(const sc2k_api *api, sc2k_mod_info *info);

#endif
