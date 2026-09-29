# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses
[Semantic Versioning](https://semver.org/).

## [Unreleased]

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
