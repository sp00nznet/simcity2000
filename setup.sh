#!/bin/bash
# Linux setup: the same pipeline as Setup.cmd. The Windows host (build/sc2k.exe)
# is cross-compiled here with clang-cl and played under Wine; there is no
# native Linux host yet.
#
#   ./setup.sh                 # asks for the game when game/ isn't there yet
#   ./setup.sh --game-dir DIR  # the game: a folder with SIMCITY.EXE, the disc
#                              # (WIN95/SC2K inside), an .iso, or a folder with one
#   ./setup.sh --yes           # install what is missing without asking
#   ./setup.sh --force         # redo every step
#
# It copies the game into game/, makes the function catalog, lifts, builds,
# and leaves "SimCity 2000 (recomp).sh" here. A rerun skips finished steps.
set -e
cd "$(dirname "$0")"
ROOT=$PWD
FORCE=0; YES=0; GAME_ARG=""
while [ $# -gt 0 ]; do
  case "$1" in
    --force) FORCE=1 ;;
    --yes) YES=1 ;;
    --game-dir) GAME_ARG=$2; shift ;;
    *) echo "usage: ./setup.sh [--game-dir DIR] [--yes] [--force]" >&2; exit 2 ;;
  esac
  shift
done

say()  { printf '%s\n' "$*"; }
# "[n/N] what": the installer's progress bar reads these.
STEP=0; STEPS=6
step() { STEP=$((STEP + 1)); printf '\n\033[36m[%d/%d] %s\033[0m\n' "$STEP" "$STEPS" "$*"; }
fail() { printf '\n\033[31mSetup stopped: %s\033[0m\n' "$*" >&2; exit 1; }
ask()  { [ "$YES" = 1 ] && { say "$1 yes"; return 0; }; read -r -p "$1 [Y/n] " r; [[ ! $r =~ ^[nN] ]]; }
done_already() { [ "$FORCE" = 0 ] && [ -e "$1" ]; }
# Debian and Ubuntu keep clang-cl and llvm-lib in /usr/lib/llvm-*/bin.
have() { command -v "$1" >/dev/null || ls /usr/lib/llvm-*/bin/"$1" >/dev/null 2>&1; }
mkdir -p work

# ---------------------------------------------------------------- tools
step "Checking the tools"
PATH=$PATH:$HOME/.local/bin
need=()
for t in clang-cl lld-link llvm-lib cmake ninja wine python3; do have "$t" || need+=("$t"); done
if [ ${#need[@]} -gt 0 ]; then
  say "  Missing: ${need[*]}. From your package manager, for example:"
  say "    Debian, Ubuntu: sudo dpkg --add-architecture i386 && sudo apt update"
  say "                    sudo apt install clang clang-tools lld llvm cmake ninja-build wine wine32:i386 python3-venv"
  say "    Fedora:         sudo dnf install clang lld llvm cmake ninja-build wine python3"
  say "    Arch:           sudo pacman -S clang lld llvm cmake ninja wine python (multilib)"
  fail "install them, then run ./setup.sh again."
fi

if ! command -v xwin >/dev/null; then
  say "  xwin (https://github.com/Jake-Shadle/xwin) fetches the MSVC C runtime and Windows SDK."
  ask "  Download its release binary into ~/.local/bin?" || fail "xwin is required (or: cargo install xwin)."
  mkdir -p "$HOME/.local/bin"
  v=0.6.6; a=$(uname -m)
  curl -fsSL "https://github.com/Jake-Shadle/xwin/releases/download/$v/xwin-$v-$a-unknown-linux-musl.tar.gz" |
    tar -xz -C "$HOME/.local/bin" --strip-components=1 "xwin-$v-$a-unknown-linux-musl/xwin" ||
    fail "could not download xwin."
fi
export XWIN_DIR=${XWIN_DIR:-$HOME/.xwin}
if [ ! -d "$XWIN_DIR/crt/lib/x86" ]; then
  say "  The x86 MSVC C runtime and Windows SDK are needed to build a Windows exe (about 1 GB)."
  say "  xwin downloads them from Microsoft, under Microsoft's licence:"
  say "  https://go.microsoft.com/fwlink/?LinkId=2086102"
  ask "  Accept that licence and download them into $XWIN_DIR?" || fail "the CRT and SDK are required."
  # The cache beside the output: moving out of /tmp fails where it is tmpfs.
  xwin --accept-license --cache-dir "$XWIN_DIR.cache" --arch x86 splat --output "$XWIN_DIR"
  rm -rf "$XWIN_DIR.cache"
fi

if python3 -c 'import pefile, capstone' 2>/dev/null; then
  PY=$(command -v python3)
else
  if ! .venv/bin/python -c 'import pefile, capstone' 2>/dev/null; then
    if command -v uv >/dev/null; then uv venv .venv && uv pip install --python .venv/bin/python pefile capstone
    else python3 -m venv .venv && .venv/bin/pip install -q pefile capstone; fi
  fi
  PY=$ROOT/.venv/bin/python
  "$PY" -c 'import pefile, capstone' 2>/dev/null ||
    fail "Python could not make its venv for pefile and capstone: install python3-venv (Debian, Ubuntu) or uv."
fi

# The toolkit: PCRECOMP, else ../pcrecomp, else ../tools, else ../pcrecomp is cloned.
if [ -z "$PCRECOMP" ]; then
  if [ ! -d ../pcrecomp ] && [ -d ../tools/runtime ]; then PCRECOMP=$(cd ../tools && pwd)
  else PCRECOMP=$(cd .. && pwd)/pcrecomp; fi
fi
if [ ! -f "$PCRECOMP/tools/lift/lift32.py" ]; then
  ask "  Clone the pcrecomp toolkit into $PCRECOMP?" || fail "pcrecomp is required."
  git clone --depth 1 https://github.com/sp00nznet/pcrecomp "$PCRECOMP"
fi
# disasm32 --full (pcrecomp#23): the catalog the lift reads.
grep -q -- '--full' "$PCRECOMP/tools/disasm/disasm32.py" ||
  fail "$PCRECOMP is too old for this build: update it (git -C \"$PCRECOMP\" pull)."
export PCRECOMP
say "  pcrecomp: $PCRECOMP"

# ---------------------------------------------------------------- the game
step "Copying SimCity 2000 into game/"
# The Win95 SIMCITY.EXE is a PE; the Windows 3.1 one (NE) is not supported.
is_win95() { [ -f "$1" ] && "$PY" -c 'import pefile, sys; pefile.PE(sys.argv[1], fast_load=True)' "$1" 2>/dev/null; }
# A folder -> the folder holding SIMCITY.EXE (itself, or WIN95/SC2K on the disc), any case.
find_exe() { find "$1" -maxdepth 3 -iname simcity.exe 2>/dev/null | while read -r e; do is_win95 "$e" && { echo "$e"; break; }; done; }
EXE=$(find_exe game)
if [ -n "$EXE" ] && [ "$FORCE" = 0 ]; then
  say "  already in game/ (skipping)"
else
  src=$GAME_ARG
  while :; do
    [ -z "$src" ] && [ "$YES" = 1 ] && fail "no game given: choose the SimCity 2000 disc, its .iso, or a folder with SIMCITY.EXE."
    [ -z "$src" ] && read -r -p "  The SimCity 2000 disc, its .iso, or a folder with the Win95 SIMCITY.EXE: " src
    [ -d "$src" ] && [ -z "$(find_exe "$src")" ] && iso=$(find "$src" -maxdepth 1 -iname '*.iso' | head -1) && [ -n "$iso" ] && src=$iso
    if [ -f "$src" ] && [[ ${src,,} == *.iso ]]; then
      say "  Unpacking $src"
      rm -rf work/disc; mkdir -p work/disc
      if command -v bsdtar >/dev/null; then bsdtar -xf "$src" -C work/disc
      elif command -v 7z >/dev/null; then 7z x -y -owork/disc "$src" >/dev/null
      else fail "bsdtar or 7z is needed to read an .iso (libarchive-tools or p7zip), or mount it and choose the folder."; fi
      src=work/disc
    fi
    exe=$(find_exe "$src")
    [ -n "$exe" ] && break
    say "  No Windows 95 SIMCITY.EXE in $src (the DOS and Windows 3.1 versions are different programs)."
    [ "$YES" = 1 ] && fail "that is not the Windows 95 SimCity 2000."
    src=""
  done
  rm -rf game; cp -r "$(dirname "$exe")" game; chmod -R u+w game
  rm -rf work/disc
  EXE=$(find_exe game)
fi
say "  $EXE"

# ---------------------------------------------------------------- lift and build
step "Finding every function (a few minutes, once)"
if done_already analysis/d32_funcs.json; then say "  done (skipping)"; else
  mkdir -p analysis
  "$PY" "$PCRECOMP/tools/disasm/disasm32.py" "$EXE" --full --output analysis/d32_funcs.json > work/disasm.log 2>&1 ||
    fail "see work/disasm.log"
fi

step "Lifting to C (a few minutes)"
if done_already src/recomp/gen/recomp_dispatch.c; then say "  done (skipping)"; else
  "$PY" run_lift.py --exe "$EXE" > work/lift.log 2>&1 || fail "see work/lift.log"
  grep "functions .* errors" work/lift.log
fi

step "Building build/sc2k.exe (5 to 15 minutes)"
# always: the build only compiles what changed, so a pulled fix reaches it
[ "$FORCE" = 1 ] && rm -rf build
./build.sh > work/build.log 2>&1 || fail "the build failed: see work/build.log"

step "Making the launcher"
launcher="SimCity 2000 (recomp).sh"
cat > "$launcher" <<EOF
#!/bin/sh
# Plays the recompiled game under Wine, in the game's own windows: Wine shows
# them over the frontend's, which hides them on a desktop of their own on Windows.
cd "\$(dirname "\$0")" && exec "\${WINE:-wine}" build/sc2k.exe --classic "$EXE" "\$@"
EOF
chmod +x "$launcher"
say "  $launcher"
printf '\n\033[32mDone.\033[0m Run "%s" to play.\n' "$launcher"
