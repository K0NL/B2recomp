# Runs the first tutorial fight (from build\saves\after-prologue) once per graphics.ini in build\gfx\*.ini, with GPU timing
# and full-resolution screenshots, into build\gfx_<name>\.  Usage: .\scripts\gfx_compare.ps1 [seconds] [names...]
param([int]$Seconds = 100, [string[]]$Names = @('s10', 's15', 's20', 's20af'))
$root = Split-Path $PSScriptRoot
foreach ($n in $Names) {
    $run = "gfx_$n"
    New-Item -ItemType Directory -Force "$root\build\$run\userdata", "$root\build\$run\shots" | Out-Null
    Copy-Item -Recurse -Force "$root\build\saves\after-prologue\*" "$root\build\$run\userdata\"
    $env:SSBU_GRAPHICS_CONFIG = "$root\build\gfx\$n.ini"
    $env:SSBU_SEQ = '@draws:65-95/30 30:Z 180:Z 150:Z 150:Z 150:Z 150:Z @draws:300-4000/120 120:Z 120:Z 120:X 300:V 30:V 30:V 600:V 30:V 30:V 600:C 30:C'
    $env:SSBU_SHOT_EVERY = '300'; $env:SSBU_SHOT_DIR = "$root\build\$run\shots"; $env:SSBU_VK_SHOT_NATIVE = '1'; $env:SSBU_VK_GPUTIME = '1'
    & "$PSScriptRoot\run_boot.ps1" $run $Seconds | Select-Object -Last 1
}
Remove-Item Env:SSBU_GRAPHICS_CONFIG, Env:SSBU_VK_SHOT_NATIVE, Env:SSBU_VK_GPUTIME -ErrorAction SilentlyContinue
