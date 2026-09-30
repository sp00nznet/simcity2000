# Mods

A mod is a name, a description and a `tick` function that runs about 20 times
a second while the mod is switched on. Some are built in; the rest are DLLs.
Switch them from the frontend's **Mods** menu (remembered in `sc2k.ini`), or
with `SC2K_MODS="Always sunny,City stipend"`. They run headless too.

## Built in

| Mod | What it does |
|---|---|
| Always sunny | Locks the weather to Clear. The simulation reads the weather (fires spread in heat and wind), so this is a gameplay change as well as a look. |

## Writing one

Put a 32-bit DLL in `mods\` beside `sc2k.exe` that exports
`sc2k_mod_register` (`src/runtime/sc2k_mod.h`):

```c
#include "sc2k_mod.h"

static const sc2k_api *api;

static void tick(void) {
    if (api->funds() < 1000) api->set_funds(1000);      /* never quite broke */
}

__declspec(dllexport) int sc2k_mod_register(const sc2k_api *a, sc2k_mod_info *info) {
    if (a->version != SC2K_MOD_API) return 0;
    api = a;
    info->name = "Safety net";
    info->description = "Funds never fall below $1,000.";
    info->tick = tick;
    return 1;
}
```

The API gives game memory by the original `SIMCITY.EXE` address
(`api->game(va)`), a lock to write it under, the clock, funds and a log.
[effects.md](effects.md) lists the addresses known so far.

`mods/stipend` is the worked example, built with the project into
`build\mods\stipend.dll`: the state pays the city $500 at the start of each game
month. Checked headless with turbo: eleven payments in twenty seconds, one per
game month.

## What is not here yet

- **Replacing game functions.** Every game function is C, so a mod could swap
  one out -- a free-building mod would replace the spending routine. The lift
  would need to emit hookable entry points for the chosen functions; see the
  roadmap.
- **The game's own cheat codes.** Its cheat-code matcher (`0x0040CA30`) checks
  typed keys against `fund`, `cass`, `ieatmach`, `naho`, `nowusehe`,
  `pallicsir`, `grantmili` and `joke`. Typed at the city view in a headless run
  they did nothing visible, so they are not in the menus until what triggers
  them is understood.
