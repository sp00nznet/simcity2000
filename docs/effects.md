# Effects

Everything here is optional, and nothing in it changes the game's code or its
simulation: it changes how the host shows what the game drew.

## Day and night, seasons, weather (`fx.c`)

The city is drawn in 256 colours, and the map view's colour tables pass
through the host (`palette.c`). `fx.c` recolours each **palette entry** on its
way to the screen. Working per entry is what makes night look right: an entry
that is a light -- bright and in the range the game already animates for
traffic and signals, or a lit window's yellow -- keeps its brightness while
the rest darkens toward moonlight blue, with an orange band at dusk.

It runs on the game's own state, read from its memory:

| What | Where | Meaning |
|---|---|---|
| Day counter | `0x004CAE04` (int32) | 25 days a month, 300 a year; month = `(d / 25) % 12` |
| Weather | `0x004CB40C` (byte) | 0 Cold, 1 Clear, 2 Hot, 3 Foggy, 4 Chilly, 5 Overcast, 6 Snow, 7 Rain, 8 Windy, 9 Blizzard, 10 Hurricane, 11 Tornado |
| Funds | `0x004CA444` (int32) | what the title bar shows |

Found by reading the code that builds the title bar (`"%s %4d <%s> %s"`) and
the status bar's weather pane, and checked against both on screen.

The sky follows the game clock, so it stops when the game is paused and
hurries at African Swallow. One day-and-night lasts `cycle_days` game days (50
by default: two game months).

Seasons: winter is paler and cooler, summer warmer, and in September to
November green entries turn toward orange. Weather: fog lifts toward grey,
overcast desaturates, rain darkens, snow and blizzards whiten, storms go dark.

Settings, until the in-game menu has them:

| Variable | Effect |
|---|---|
| `SC2K_FX=daynight,seasons,weather` | which effects are on (`none` for none) |
| `SC2K_DAYCYCLE=N` | game days per day-and-night |

`sc2k --selftest` checks the effect math without starting the game.
