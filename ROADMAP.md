# Roadmap

## Next

- **Speed.** The recompiled simulation falls about a month behind the original
  over five minutes. Profile the hot sim loops; try `RECOMP_LOCAL_REGS`.
- **A city that grows**, as a scripted scenario, to watch growth and traffic
  against `--native`.
- **The scenarios** (`Load Scenario`) and the disasters menu.
- **Sound and music**: `sndPlaySound` and MIDI through MCI, both pass through
  today and have not been checked.
- **`Setup.cmd` quick start** (REPO_RULES section 5): check prerequisites, find
  the disc or ISO, run the steps below, leave a shortcut.
- **Conformance harness** (REPO_RULES section 9): a fixed set of scripted
  headless runs with a pass count: "reaches the menu", "builds and saves a
  city", "NYC after five minutes has the original's funds". `--native` gives
  the reference values on the same machine, so no captures need to ship.

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
