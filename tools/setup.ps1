# One-click setup for the SimCity 2000 static recompilation. Run it by
# double-clicking Setup.cmd; the README's "Step by step" is the same thing by hand.
#
# It installs what is missing, copies the game into game\, makes the function
# catalog, lifts, builds build\sc2k.exe and leaves "SimCity 2000 (recomp).cmd".
# Each step is skipped when its output exists (-Force redoes them). -Yes
# installs what is missing without asking; -Game is the disc's drive, an .iso,
# a folder holding one, or a folder with the Win95 SIMCITY.EXE. Everything goes
# to setup.log. RECOMP_LAUNCHER=1 (the recomp launcher) skips "Press Enter".
param([switch]$Force, [switch]$Yes, [Alias('GameDir')][string]$Game = "")

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
# The toolkit: PCRECOMP, else ../pcrecomp, else ../tools, else ../pcrecomp is cloned.
$Parent = Split-Path -Parent $Root
$Toolkit = if ($env:PCRECOMP) { $env:PCRECOMP }
  elseif (-not (Test-Path (Join-Path $Parent 'pcrecomp')) -and (Test-Path (Join-Path $Parent 'tools\tools\lift'))) { Join-Path $Parent 'tools' }
  else { Join-Path $Parent 'pcrecomp' }
$T = Join-Path $Toolkit 'tools'
$Log = Join-Path $Root 'setup.log'
Set-Location $Root
$Steps = 6
function Log($t) { try { Add-Content -Path $Log -Value $t -Encoding UTF8 -ErrorAction Stop } catch {} }
Log "==== setup $(Get-Date -Format s)"
function Say($t, $c = 'Gray') { Write-Host $t -ForegroundColor $c; Log $t }
function Pause-End { if (-not $env:RECOMP_LAUNCHER) { Read-Host "Press Enter to close" | Out-Null } }
# cmd.exe, which the build runs through, splits a path at '&'.
if ($Root -match '&') {
  Say "This folder's path has an '&' in it, which the build cannot handle: $Root" 'Red'
  Say "Setup stopped: move the folder (for example to C:\simcity2000) and run Setup.cmd from there." 'Red'
  Pause-End; exit 1
}
function Step($n, $t) { Write-Host ""; Say "[$n/$Steps] $t" 'Cyan' }
function Fail($t) {
  Say ""; Say "Setup stopped: $t" 'Red'
  Say "The details are in $Log. Fix that and run Setup.cmd again; finished steps are skipped." 'Yellow'
  Pause-End; exit 1
}
function Ask($q) { if ($Yes) { Say "$q yes"; return $true }; $a = Read-Host "$q [Y/n]"; return -not ($a -match '^[nN]') }
function Refresh-Path {
  $env:Path = [Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' + [Environment]::GetEnvironmentVariable('Path', 'User')
}
# A native program with its output in the log. Windows PowerShell turns its
# stderr into errors, so 'Stop' is off while it runs.
function Exec([string[]]$cmd) {
  $old = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
  $rest = @($cmd | Select-Object -Skip 1)
  & $cmd[0] @rest 2>&1 | ForEach-Object { Log "$_" }
  $code = $LASTEXITCODE
  $ErrorActionPreference = $old
  return $code
}
function Run($what, [string[]]$cmd) {
  Say "  $what..."
  $code = Exec $cmd
  if ($code -ne 0) { Fail "$what failed (exit code $code)." }
}
function Winget($id, [string[]]$extra = @()) {
  if (-not (Get-Command winget -ErrorAction SilentlyContinue)) { Fail "winget is missing. Install 'App Installer' from the Microsoft Store." }
  Exec (@('winget', 'install', '-e', '--id', $id, '--accept-package-agreements', '--accept-source-agreements') + $extra) | Out-Null
  Refresh-Path
}

if (-not $env:RECOMP_LAUNCHER) { Clear-Host }
Say "SimCity 2000 - static recompilation setup" 'White'
Say "You need the Windows 95 SimCity 2000 (the CD Collection or Special Edition disc, or its .iso) and about 3 GB free."

# ---------------------------------------------------------------- tools
Step 1 "Checking the tools"
# A Python that answers "Python 3.x" (the Store's placeholder answers nothing).
function Find-Python {
  foreach ($c in @(@('py', '-3'), @('python'), @('python3'))) {
    if (-not (Get-Command $c[0] -ErrorAction SilentlyContinue)) { continue }
    $old = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    $rest = @($c | Select-Object -Skip 1)
    $v = (& $c[0] @rest --version 2>&1 | Out-String).Trim()
    $ErrorActionPreference = $old
    if ($v -match '^Python 3\.(\d+)' -and [int]$Matches[1] -ge 10) { return ,$c }
  }
  return $null
}
$py = Find-Python
if (-not $py) {
  if (-not (Ask "  Python 3 is not installed. Install Python 3.12 now (winget, for your user only)?")) { Fail "Python 3 is required." }
  Winget 'Python.Python.3.12' @('--scope', 'user')
  $py = Find-Python
  if (-not $py) { Fail "Python installed, but Windows has not picked it up yet: close this window and run Setup.cmd again." }
}
Say "  Python: $($py -join ' ')"
if ((Exec ($py + @('-c', 'import pefile, capstone'))) -ne 0) {
  if (-not (Ask "  Install the Python packages pefile and capstone (pip, for your user only)?")) { Fail "pefile and capstone are required." }
  Run "Installing pefile and capstone" ($py + @('-m', 'pip', 'install', '--user', 'pefile', 'capstone'))
}

if (-not (Test-Path (Join-Path $T 'lift\lift32.py'))) {
  if (-not (Get-Command git -ErrorAction SilentlyContinue)) { Fail "git is needed to fetch pcrecomp. Install Git for Windows, then run Setup.cmd again." }
  if (-not (Ask "  Download the pcrecomp toolkit into $Toolkit (git)?")) { Fail "pcrecomp is required at $Toolkit." }
  Run "Cloning pcrecomp" @('git', 'clone', '--depth', '1', 'https://github.com/sp00nznet/pcrecomp', $Toolkit)
}
# disasm32 --full (pcrecomp#23): the catalog the lift reads.
if (-not (Select-String -Quiet -SimpleMatch -- '--full' (Join-Path $T 'disasm\disasm32.py'))) {
  Fail "your pcrecomp at $Toolkit is too old for this build. Update it: git -C `"$Toolkit`" pull"
}
$env:PCRECOMP = $Toolkit
Say "  pcrecomp: $Toolkit"

# Visual Studio (x86 libraries, CMake and Ninja come with it) and LLVM's clang-cl.
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
function Find-VS { if (Test-Path $vswhere) { & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath } }
$vs = Find-VS
if (-not $vs) {
  if (-not (Ask "  Install the Visual Studio 2022 Build Tools with C++ and CMake (winget, about 3 GB, asks for admin)?")) {
    Fail "Visual Studio 2022 (or its Build Tools) with 'Desktop development with C++' is needed."
  }
  Winget 'Microsoft.VisualStudio.2022.BuildTools' @('--override', '--passive --wait --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.CMake.Project --includeRecommended')
  $vs = Find-VS
  if (-not $vs) { Fail "the build tools did not finish installing. Run Setup.cmd again once the Visual Studio Installer is done." }
}
Say "  Visual Studio: $vs"
$vsCMake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake'
foreach ($tool in @(@('cmake', 'Kitware.CMake', 'CMake\bin'), @('ninja', 'Ninja-build.Ninja', 'Ninja'))) {
  if ((Get-Command $tool[0] -ErrorAction SilentlyContinue) -or (Test-Path (Join-Path $vsCMake "$($tool[2])\$($tool[0]).exe"))) { continue }
  if (-not (Ask "  $($tool[0]) is not installed. Install it now (winget)?")) { Fail "$($tool[0]) is required." }
  Winget $tool[1]
}
if (-not (Test-Path "$env:ProgramFiles\LLVM\bin\clang-cl.exe")) {
  if (-not (Ask "  LLVM (clang-cl) is not installed. Install it now (winget, about 400 MB, asks for admin)?")) { Fail "LLVM is required." }
  Winget 'LLVM.LLVM'
  if (-not (Test-Path "$env:ProgramFiles\LLVM\bin\clang-cl.exe")) { Fail "LLVM did not install to $env:ProgramFiles\LLVM." }
}
Say "  LLVM: $env:ProgramFiles\LLVM"

# ---------------------------------------------------------------- the game
Step 2 "Copying SimCity 2000 into game\"
# The Win95 SIMCITY.EXE is a PE; the Windows 3.1 one (NE) is not supported.
function Is-Win95([string]$e) { (Test-Path $e) -and (Exec ($py + @('-c', 'import pefile, sys; pefile.PE(sys.argv[1], fast_load=True)', $e))) -eq 0 }
# A folder -> the folder holding the Win95 SIMCITY.EXE (itself, or WIN95\SC2K on the disc), or "".
function Find-Game([string]$d) {
  foreach ($s in @('', 'WIN95\SC2K', 'SC2K')) {
    $e = Join-Path (Join-Path $d $s) 'SIMCITY.EXE'
    if (Is-Win95 $e) { return (Split-Path -Parent (Resolve-Path $e).Path) }
  }
  return ""
}
$script:mounted = $null
# A drive letter, a folder, or an .iso (mounted here) -> the game's folder, or "".
function Resolve-Game([string]$d) {
  $d = $d.Trim('"', ' ')
  if (-not $d) { return "" }
  if ($d -match '^[A-Za-z]:?\\?$') { $d = "$($d.Substring(0,1)):\" }
  if ((Test-Path $d -PathType Container) -and -not (Find-Game $d)) {
    $iso = Get-ChildItem $d -Filter *.iso -File -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($iso) { $d = $iso.FullName }
  }
  if ($d -match '\.iso$' -and (Test-Path $d)) {
    try {
      $iso = (Resolve-Path $d).Path
      $img = Mount-DiskImage -ImagePath $iso -PassThru
      $script:mounted = $iso
      $d = "$(($img | Get-Volume).DriveLetter):\"
      Say "  Mounted the .iso as $d"
    } catch {
      Say "  Windows would not mount it from here. Double-click the .iso in Explorer, then give that drive's letter." 'Yellow'
      return ""
    }
  }
  $g = Find-Game $d
  if (-not $g) { Say "  No Windows 95 SIMCITY.EXE in $d (the DOS and Windows 3.1 versions are different programs)." 'Yellow' }
  return $g
}

if ((Is-Win95 'game\SIMCITY.EXE') -and -not $Force) {
  Say "  Already in game\ (skipping)."
} else {
  $src = Resolve-Game $Game
  if (-not $src) {
    foreach ($v in @(Get-Volume -ErrorAction SilentlyContinue | Where-Object { $_.DriveType -eq 'CD-ROM' -and $_.DriveLetter })) {
      $src = Find-Game "$($v.DriveLetter):\"
      if ($src) { Say "  Found the disc in $($v.DriveLetter):"; break }
    }
  }
  if (-not $src -and $Yes) { Fail "SimCity 2000 was not found. Choose its disc, its .iso, or a folder with the Win95 SIMCITY.EXE." }
  while (-not $src) { $src = Resolve-Game (Read-Host "  The disc's drive letter (e.g. E), an .iso, or a folder with the Win95 SIMCITY.EXE") }
  Say "  Copying $src"
  $code = Exec @('robocopy', $src, (Join-Path $Root 'game'), '/E', '/NFL', '/NDL', '/NJH', '/NP', '/XD', $Root)
  if ($script:mounted) { Dismount-DiskImage -ImagePath $script:mounted | Out-Null }
  if ($code -ge 8) { Fail "copying the game failed (robocopy exit code $code)." }
  # Files from a disc are read-only; the game saves beside itself.
  Get-ChildItem game -Recurse -File | ForEach-Object { $_.IsReadOnly = $false }
}

# ---------------------------------------------------------------- lift and build
Step 3 "Finding every function (a few minutes, once)"
if ((Test-Path 'analysis\d32_funcs.json') -and -not $Force) { Say "  Already done (skipping)." }
else {
  New-Item -ItemType Directory -Force analysis | Out-Null
  Run "Disassembling" ($py + @("$T\disasm\disasm32.py", 'game\SIMCITY.EXE', '--full', '--output', 'analysis\d32_funcs.json'))
}

Step 4 "Lifting to C (a few minutes)"
if ((Test-Path 'src\recomp\gen\recomp_dispatch.c') -and -not $Force) { Say "  Already done (skipping)." }
else { Run "Lifting" ($py + @('run_lift.py')) }

Step 5 "Building build\sc2k.exe (5 to 15 minutes)"
# Always: the build only compiles what changed, so a pulled fix reaches sc2k.exe.
if ($Force -and (Test-Path build)) { Remove-Item -Recurse -Force build }
Run "Compiling" @('cmd', '/c', ('call "' + (Join-Path $Root 'tools\build.cmd') + '"'))

Step 6 "Making the shortcut"
$lnk = Join-Path $Root 'SimCity 2000 (recomp).cmd'
Set-Content -Path $lnk -Encoding ASCII -Value @(
  '@echo off',
  'rem Plays the recompiled game. --classic for the game''s own windows.',
  'cd /d "%~dp0"',
  'start "" build\sc2k.exe game\SIMCITY.EXE %*')
Say "  $lnk"
Write-Host ""
Say "Done. Double-click 'SimCity 2000 (recomp).cmd' to play." 'Green'
Pause-End
