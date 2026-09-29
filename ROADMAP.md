# Roadmap

## Next

- **Play a city.** Close the newspaper, build with the toolbar, let the
  simulation run for a few game years, and see what breaks.
- **Palette animation.** The game animates water, lights and traffic by
  rewriting palette entries, which does nothing on a 32-bit desktop. Emulate an
  8-bit palette in the host (the blits already go through GDI calls we can see).
- **Save and load** a city; load the bundled cities and scenarios.
- **Sound and music**: `sndPlaySound` and MIDI through MCI, both pass through
  today and have not been checked.
- **`Setup.cmd` quick start** (REPO_RULES section 5): check prerequisites, find
  the disc or ISO, run the steps below, leave a shortcut.
- **Conformance harness** (REPO_RULES section 9): a fixed set of scripted
  headless runs with a pass count, e.g. "reaches the menu", "generates terrain",
  "January 1900 newspaper", compared frame-for-frame against reference captures
  kept outside the repo.

## Later

- An IDA-free function catalog (pcrecomp `disasm32.py`), so building does not
  need a commercial disassembler. Score it against the IDA catalog first.
- The intro movie: point the CD check at a folder instead of a drive.
- C++ exception dispatch through the guest's own SEH chain. MFC throws on
  errors such as a bad file; today such a throw reaches the host unhandled.
- Toolkit PRs: the KWAJ and Knowledge Dynamics/LZHUF extractors written while
  finding this build, and the generic Win32 pass-through bridge and DEP
  callback trap from `host.c`, which suit any 32-bit Win32 title.

## Out of scope

- The DOS and Windows 3.1 versions (different binaries: DOS/4GW LE and Win16
  NE). pcrecomp has no LE front end yet; see Terminal Velocity.
- SCURK (`WINSCURK.EXE`), the building editor. A separate program.
