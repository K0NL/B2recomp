# Play the game yourself: no input script, no time limit, window and sound on, saves kept in build\play\userdata.
# The first start copies build\saves\after-prologue (slot 1 at the first chapter) into it; -NewSave starts from no save.
# Controllers: any pad SDL knows (DualSense, Xbox, Switch Pro, ...) as player 1; it is read while the game window has the
# focus. Buttons by position (Nintendo layout): right = A, bottom = B, top = X, left = Y (config\controllers.ini).
# Keyboard: arrows = left stick, Z X C V = A B X Y, Q E = L R, A S = ZL ZR, Enter = Plus, Backspace = Minus.
# build\game_native.exe is the llvm-mingw build (the default toolchain since finding 101); -Msvc plays the clang 9 + MSVC
# build in build\out_msvc (TOOLCHAIN=msvc) instead: same saves, its log is build\play\stdout_msvc.txt.
# Usage: .\scripts\play.ps1 [-NewSave] [-Msvc]
param([switch]$NewSave, [switch]$Msvc)
$root = Split-Path $PSScriptRoot
$exe = Join-Path $root $(if ($Msvc) { 'build\out_msvc\game_native.exe' } else { 'build\game_native.exe' })
if (-not (Test-Path $exe)) {
    "Not found: $exe"
    'Build it first: ' + $(if ($Msvc) { '$env:TOOLCHAIN=''msvc''; ' } else { '' }) + 'python scripts\compile_main.py 16; python scripts\build_runtime.py'
    exit 1
}
$user = Join-Path $root 'build\play\userdata'
if ($NewSave -and (Test-Path $user)) { Remove-Item -Recurse -Force $user }
if (-not (Test-Path "$user\save")) {
    New-Item -ItemType Directory -Force $user | Out-Null
    if (-not $NewSave -and (Test-Path "$root\build\saves\after-prologue\save")) { Copy-Item -Recurse -Force "$root\build\saves\after-prologue\*" "$user\" }
}
# a build running at the same time (16 compiler jobs) starves the game: seconds without a frame (finding 97)
$busy = @(Get-Process clang, lld-link, ld.lld -ErrorAction SilentlyContinue)
if ($busy.Count) { "Warning: $($busy.Count) compiler processes are running (a build in this or another project): expect stalls until it ends" }
foreach ($v in 'SSBU_SEQ', 'SSBU_SCRIPT', 'SSBU_PRESS', 'SSBU_SHOT', 'SSBU_SHOT_EVERY', 'SSBU_SHOT_DIR', 'SSBU_PROF15', 'SSBU_HITCH', 'SSBU_TRACE') { Remove-Item "Env:$v" -ErrorAction SilentlyContinue }
$env:SSBU_ROMFS = Join-Path $root 'targets\romfs'
$env:SSBU_USERDATA = $user
$env:SSBU_SHOW = '1'; $env:SSBU_VOLUME = '100'
# keep running when another window is in front: nnmisc.c otherwise pauses the game like the console's HOME button, which on a
# PC froze play whenever any window took the focus (the pad is still read only while the game window has the focus)
$env:SSBU_FOCUS_NOTIFY = '0'
# the settings by name: the runtime looks for <exe folder>\..\config, which for build\out_msvc is build\config (finding 97)
$env:SSBU_GRAPHICS_CONFIG = Join-Path $root 'config\graphics.ini'
$env:SSBU_CONFIG = Join-Path $root 'config\controllers.ini'
$fonts = Join-Path $root 'targets\sysdata\fonts'
if (Test-Path $fonts) { $env:SSBU_FONTS = $fonts }
$log = Join-Path $root $(if ($Msvc) { 'build\play\stdout_msvc.txt' } else { 'build\play\stdout.txt' })
# cmd writes the log; no console window (a hidden start state would also be applied to the game's window)
$psi = New-Object Diagnostics.ProcessStartInfo 'cmd.exe', "/c `"`"$exe`" `"$(Join-Path $root 'targets\exefs\main.elf')`" > `"$log`" 2>&1`""
$psi.WorkingDirectory = Join-Path $root 'build'
$psi.UseShellExecute = $false
$psi.CreateNoWindow = $true
[Diagnostics.Process]::Start($psi) | Out-Null
"B2recomp is starting ($(if ($Msvc) { 'clang 9 + MSVC build' } else { 'llvm-mingw build' })); log: $log"
