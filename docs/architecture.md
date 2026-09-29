# Architecture

Three parts: the lifter, which turns `SIMCITY.EXE` into C; the generated C;
and the host runtime, which loads the original image and connects the
generated code to Windows.

```
SIMCITY.EXE ──IDA──> analysis/ida_funcs.json, ida_codemap.json
     │                          │
     └──────── run_lift.py <────┘   (pcrecomp lift32 + generate.py)
                    │
                    v
           src/recomp/gen/*.c  (gitignored)
                    │
   src/runtime/*.c ─┴─> build/sc2k.exe ──maps──> SIMCITY.EXE at 0x00400000
```

## The lifter (`run_lift.py`)

Function entries come from IDA's catalog, from pcrecomp's `recover_functions`
(tail-call targets and stored function pointers IDA missed), and from the
`.reloc` table: every absolute pointer from a data section into `.text` is a
code address somebody can reach indirectly. That last source finds MFC
message-map handlers and the C++ exception-unwind funclets that live inside
other functions.

Inside IDA's functions, instructions are decoded only at IDA's instruction
heads. MSVC keeps jump tables inside `.text` (`_memcpy` does
`jmp [edx*4 + table]` and the table follows the jump); a linear sweep decodes
the table as instructions, runs over the real targets, and the switch never
lands. The shared linear sweep is kept for entries IDA does not know.

## The host (`src/runtime/`)

`host.c` holds the design; `overrides.c` the few APIs that cannot pass
through; `capture.c` headless mode, recording and scripted input.

### One address space

The host is a **32-bit** program. That is the whole trick: SIMCITY.EXE is a
32-bit Windows program, so if the host is one too, every pointer the game
holds is a valid host pointer, and every Win32 structure it builds is already
laid out the way the real API expects. Nothing is translated.

The original image is mapped by the Windows loader
(`LoadLibraryEx(DONT_RESOLVE_DLL_REFERENCES)`) at its own base, `0x00400000`.
Going through the loader rather than copying sections by hand is what makes
`HINSTANCE` work: MFC loads every dialog, menu, bitmap and string from the
game's resources through ordinary API calls.

Getting `0x00400000` takes a relaunch. By the time `main` runs, process
start-up has put NLS tables and heap there, so the host starts itself again
suspended, reserves the range in the child with `VirtualAllocEx`, and resumes
it (the approach is gta's `premap.c`). The child releases the reservation and
maps the game into it before anything else can allocate.

### Guest to native

The lifted code keeps x86 registers in globals and the stack in guest memory.
A call through the IAT lifts to `RECOMP_ICALL(MEM32(iat_slot))`, and the IAT
holds the real API addresses, as the Windows loader would have left it. Any
dispatch to an address outside the game image that is executable host memory
goes to `native_bridge`, which:

1. copies 32 argument words from the guest stack to the host stack,
2. calls the function (`native_call32`, a few lines of assembly),
3. measures how far the callee moved the stack pointer — that is how many
   bytes of arguments it popped, `stdcall` or `cdecl` alike,
4. writes `eax`/`edx` back and pops the guest stack to match.

So there is no table of argument counts for the 445 imports, and overriding
one is just writing an ordinary `WINAPI` C function and putting its address in
the IAT slot (`overrides.c`).

### Native to guest

Windows calls into the game constantly: window procedures, MFC's CBT hook,
dialog procedures, timers, thread start routines. Rather than list them, the
game's `.text` is mapped non-executable and DEP is on. When Windows jumps to a
game address the CPU faults on the execute; the vectored handler sees an
execute fault inside `.text`, and redirects to `callback_entry` with the
target in `eax`. That copies the arguments onto the guest stack, runs the
lifted function, and returns to Windows popping exactly what the lifted
`ret N` popped.

### Threads

The game uses `CreateThread` and `timeSetEvent`, and the lifted code's
registers are globals. A guest lock serialises guest execution. Every native
call saves the register file, releases the lock, calls, re-acquires, and
restores. A thread that enters guest code for the first time (a timer thread,
a new thread's start routine) gets its own guest stack and its own simulated
TIB for `fs:`.

The lock is released only around native calls. Guest code that spins waiting
for another guest thread without calling Windows would deadlock; nothing has
yet.

## Palette animation (`palette.c`)

The game draws into 8-bit DIBs and animates traffic, lights and the like with
`AnimatePalette`, which only changes the screen on a 256-colour palette
display. Told the truth about a 32-bit desktop, it switches to fixed-colour
tiles and warns that "some animations won't display correctly".

So the host tells it what it expects: `GetDeviceCaps` on a display DC reports
a palette device (`RC_PALETTE`, 8 bits per pixel, 256 entries, 20 reserved).
Then the host plays the palette hardware. Every `BitBlt`/`StretchBlt` from an
8-bit DIB to a window is remembered, one per destination rectangle, in order.
When the game calls `AnimatePalette`, the new entries are written into each
remembered source DIB's colour table and the remembered blits are replayed,
oldest first. On a real 8-bit display none of this runs.

## The File dialogs (`overrides.c`)

MFC 4 attaches a `CFileDialog` to the first window created on the thread after
it arms its CBT hook. On Windows 95 that was the dialog. Today's common dialog
starts COM first, so the first window is COM's hidden `OleMainThreadWndClass`,
and MFC attaches the dialog object to it; the first message to the real dialog
then dereferences a `CWnd` that does not exist. The original `SIMCITY.EXE`
crashes the same way on Windows 11.

For the length of each `GetSaveFileNameA`/`GetOpenFileNameA` call the host puts
a second CBT hook in front of MFC's that hides every window creation except a
dialog's (`#32770`), so MFC sees what it saw in 1996. The game's filter string
is also padded with spaces (`" *.sc2 "`), which Windows 95 ignored and today
turns into a pattern that matches nothing and an extension of `"sc2 "`; the
host hands the dialog a trimmed copy.

## Scripted input and the oracle (`input.c`, `--native`)

`--input` posts mouse and keyboard messages; `SendInput` only reaches the input
desktop. The game also reads the live cursor and button state while a tool is
dragged (`GetCursorPos`, `GetKeyState`, `GetAsyncKeyState`, `GetMessagePos`),
and the real cursor never moves in a headless run, so while a script runs
those four report a synthetic cursor that moves with the posted messages.
Windows are hit-tested by the script itself, walking the game's windows in
z-order, because `WindowFromPoint` kept answering with the main frame while a
modal dialog was on top.

`--native` runs the original `SIMCITY.EXE` on the same desktop under the same
recorder and script (in a job object, so it ends with the host). It is how the
recompiled simulation was checked: the same saved city, run for the same time
in both, ends with the same funds.

## Headless mode and recording (`capture.c`)

`--headless` creates a private Win32 desktop and puts the game thread on it.
Its windows exist, get messages and paint, but are never shown on the desktop
you are looking at, which is what makes it safe to run over RDP.

`--record` composites every visible top-level window of the game with
`PrintWindow` ten times a second and pipes raw frames to ffmpeg; when a capture
is slow it repeats the frame so the video keeps wall-clock time.

Headless, the game's frame is shown at a fixed 1600x960 instead of maximized,
so scripted coordinates do not depend on the attached session's resolution.
