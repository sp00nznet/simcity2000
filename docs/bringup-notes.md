# Bring-up notes

The problems that cost the most time, in the order they were hit. Each one is
a thing the next 1990s Win32 port will hit too.

## The installer compresses with something `expand.exe` no longer reads

Not needed for the Windows 95 build, but found on the way. The DOS CD packs its
files in Knowledge Dynamics installer archives (Okumura LZHUF; INSTALL.EXE
links `UNLZHUF.C`), and the Windows 3.1 floppies use MS-Setup **KWAJ method 3**,
which Windows 11's `expand.exe` silently copies without decompressing. KWAJ's
LZH mode reads **six** 4-bit table types before the tables, not five — the
sixth is padding for byte alignment (libmspack `kwajd.c`).

## 0x00400000 is taken before `main` runs

Symptom:

```
[host] could not map ...\SIMCITY.EXE at 0x00400000 (got 06390000, error 5);
that range holds a region at 00400000+0xFFB000, state 0x2000 type 0x20000
```

That one was the host's own 16 MB main-thread stack, which the linker reserves
low. With a default stack it becomes a mapped NLS file instead:

```
that range holds a region at 00400000+0x11000, state 0x1000 type 0x40000
```

Fixes, both in `host.c`: keep the host's main stack small and run the game on
its own thread with a 64 MB stack; and relaunch the host suspended with the
range reserved (gta's `premap.c` approach). One more: after the child releases
the reservation, *nothing* may allocate before the game is mapped. Starting the
recorder thread first put a 4 KB page at `0x00400000`.

## `m_nCmdShow | 3` is `SW_FORCEMINIMIZE`

The game created its main frame and all its palettes and showed nothing. The
frame was 480×20 and hidden. In the API trace:

```
[api] 00405650 ShowWindow              (02011846 0000000B ...)
```

The code is `ShowWindow(hwnd, m_nCmdShow | 3)` — "maximized, whatever we were
asked for", written when Explorer always passed `SW_SHOWNORMAL` (1). A program
started from a console gets `SW_SHOWDEFAULT` (10), and `10 | 3` is 11, which
Windows 2000 later defined as `SW_FORCEMINIMIZE`. The host now launches the
game with `STARTF_USESHOWWINDOW` / `SW_SHOWNORMAL` unless the caller chose a
show mode.

## "Not properly registered"

The game reads its settings from `HKCU\Software\Maxis\SimCity 2000` (MFC's
`SetRegistryKey`), and refuses to start without `Registration\Mayor Name`. The
host writes the installer's defaults on first run, only for values that are
absent. `SC2K_REGTRACE=1` shows every key the game touches.

## clang-cl targets x64 whatever `vcvars32` says

`vcvars32.bat` sets up the x86 libraries but `clang-cl` still defaults to
x86-64, so CMake's compiler check links with `/machine:x64` and fails on
`mainCRTStartup`. `tools\build.cmd` passes
`-DCMAKE_C_COMPILER_TARGET=i686-pc-windows-msvc`. clang-cl also reads
top-level `__asm__` as Intel syntax; the two trampolines start with
`.att_syntax`.

`ProgramFiles(x86)` does not survive being passed through a POSIX shell (the
name is not a valid shell variable), so `build.cmd` falls back to the literal
path for `vswhere.exe`.

## `div cl` is not a 32-bit divide

Closing the first newspaper started the simulation, which faulted at once:

```
[host] exception 0xC0000005 at 6025AACC (read 0x00000001)
  in lifted 0x0046A840  ...
```

`llvm-symbolizer --obj=build/sc2k.exe 0x6025AACC` named the generated line, and
the line's comment named the instruction. The function loads a map-row pointer
into EDX, then does `div cl` to scale a byte. `div cl` divides AX into AL and
AH and leaves EDX alone; pcrecomp's `lift32` emitted the 32-bit form, dividing
EDX:EAX and overwriting EDX with a small remainder, and the next load through
"the row pointer" read address 1. The same was true of `mul`/`imul`/`idiv` with
byte and word operands. Fixed in the toolkit (branch
`fix/lift32-narrow-muldiv`) with difftest cases against Unicorn.

That is the triage loop for any crash: the crash report names the lifted
function; `llvm-symbolizer` names the generated line; the comment on the line
is the original instruction.

## The simulation hung in `strstr`

A new city ran for a few game months and froze. `SC2K_WATCH=1` showed the
guest in `0x0048D280` with its call count no longer moving: the CRT's
`strstr`. It ends its search when `repne scasb` runs with ECX = 0, which does
no iterations and leaves the flags from the previous `repe cmpsb` (not equal).
`lift32` published its loop temporaries instead, always "equal", so the search
never ended. Fixed in the toolkit (pcrecomp#8) with difftest cases.

## Save City As crashed -- the original too

`AfxDlgProc` dereferenced NULL as soon as the Save dialog appeared.
`--native` showed the original `SIMCITY.EXE` dying at the same point on
Windows 11. `SC2K_CBTRACE=1` then showed the order of windows created after
MFC armed its CBT hook:

```
[cb] 004A4167(00000003 0973189E ...)  OleMainThreadWndClass
[cb] 004A4167(00000003 029806BC ...)  WorkerW
[cb] 004A4167(00000003 0046011C ...)  #32770
```

MFC attaches its dialog object to the first of those. The fix is in
`docs/architecture.md` (The File dialogs). The next problem was a file saved
as `testcity.sc2 .sc2`: the dialog returned `TESTCITY.sc2 ` with a trailing
space, because the game's filter pattern is `" *.sc2 "`.

## "Nothing grows" was not a bug

A small test city stayed empty for years, and it looked like a dead
simulation. The same city saved and loaded in the original game stayed empty
too, with the same funds -- that layout simply does not attract settlers in
three years. The oracle (`--native`) answers questions like this in minutes;
guessing at them does not.
