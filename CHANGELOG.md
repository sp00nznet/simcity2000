# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses
[Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added

- `--frontend`: the game in the host's own Direct3D 11 window, with upscaling
  filters (nearest, smooth, sharp bilinear, Scale2x), glow, CRT, fullscreen,
  and a Dear ImGui menu bar: the game's menus mirrored with live check marks,
  plus Graphics, Effects, Display, Debug (city stats) and Cheats (funds).
  Toolbar pop-ups appear as ImGui pop-ups. Settings persist in `sc2k.ini`.
- `NOTICE` and `LICENSES/imgui.txt` for the vendored Dear ImGui (MIT).

- Day and night, seasons and weather, recoloured per palette entry from the game's
  own clock and weather, so lights stay lit at night (`fx.c`, docs/effects.md).
- `sc2k --selftest` for the effect math; `SC2K_WATCH` also shows day, month,
  weather and funds.

- A hero GIF for the README: NYC, recompiled, with a plane over the harbour.

## [0.3.0] - 2026-09-29

### Added

- Building: power plants, power lines, zones and roads placed through scripted
  input, which now keeps a synthetic cursor for the game's drag code.
- Save City As and Load City through the game's File dialogs.
- `--input` verbs `press`, `drag`, `type`, `command`, `wait`; headless runs show
  the game frame at a fixed 1600x960.
- `--native`: run the original `SIMCITY.EXE` under the same desktop, recorder and
  script, as a reference. Same save, same run: same funds.
- `SC2K_CBTRACE` diagnostic; `PCRECOMP` names the pcrecomp checkout to build with.

### Fixed

- The File dialogs no longer crash: MFC's CBT hook attached the dialog to COM's
  hidden window (the original game crashes the same way on Windows 11).
- Saved files are no longer named `name.sc2 .sc2`: the dialog filter is trimmed.
- The simulation no longer hangs in `strstr` (needs pcrecomp#8).

- Palette animation on 32-bit desktops: the host reports a 256-colour palette
  display and replays the game's 8-bit blits when it animates the palette
  (`src/runtime/palette.c`). The "not running in 256 colors" notice no longer
  appears, so its registry pre-answer is gone.

## [0.2.0] - 2026-09-29

### Added

- Saved cities open from the command line
  (`sc2k.exe game\SIMCITY.EXE CITIES\NYC.SC2`); NYC simulates a full year
  headless with no fault.
- Line info in every build (`/Z7`), so a crash address symbolizes to a
  generated line with `llvm-symbolizer`.

### Fixed

- The simulation no longer faults after the first newspaper: pcrecomp's
  `lift32` lifted `div cl` as a 32-bit divide and overwrote EDX. Needs
  pcrecomp#6 (branch `fix/lift32-narrow-muldiv`).

## [0.1.0] - 2026-09-29

### Added

- Lift driver (`run_lift.py`): IDA catalog, pcrecomp function recovery, and
  `.reloc` code pointers as entry sources; decoding at IDA's instruction heads
  so jump tables in `.text` are not decoded as code. 7,045 functions, 0 errors.
- Host runtime: the game image mapped at 0x00400000 through the Windows loader,
  imports bound to the real Win32 API with a generic pass-through bridge, and
  Windows-to-game callbacks caught with a DEP execute trap.
- Guest lock and per-thread guest stacks for the game's timer and worker threads.
- Registry defaults the installer would have written, under HKCU.
- `--headless`, `--record`, `--seconds`, `--input`; `SC2K_APITRACE`,
  `SC2K_WATCH`, `SC2K_REGTRACE`, `SC2K_CAPTRACE` diagnostics.
- Reaches the title screen, generates a new map, and starts a city.
