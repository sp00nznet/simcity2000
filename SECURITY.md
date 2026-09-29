# Security

## What the program touches

- **The registry.** `sc2k.exe` writes the SimCity 2000 settings the installer
  would have written, under `HKEY_CURRENT_USER\Software\Maxis\SimCity 2000`,
  and only values that are absent. The game reads and writes its own settings
  there too; the host redirects any `HKEY_LOCAL_MACHINE` access by the game to
  `HKEY_CURRENT_USER`, so it never needs administrator rights.
- **Files.** The game reads and writes cities in its own folder, through the
  standard Windows File dialogs.
- **Processes.** The host starts one copy of itself (to reserve the game's
  address range before startup) and, with `--native`, the original game;
  `--record` starts `ffmpeg` from `PATH`.

It makes no network connections, and handles no credentials.

## How it runs code

The game's code is compiled C running in-process, and calls the real Win32 API
with the game's arguments. A malformed city file is parsed by the game's own
1996 code, so it should be treated with the caution due any old program opening
untrusted files.

## Reporting

Report a problem privately through GitHub's *Report a vulnerability* on this
repository, rather than in a public issue.
