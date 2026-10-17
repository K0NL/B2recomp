# Run the game under the host runtime with the boot trace on, and capture stdout, stderr and the exit code.
# Usage: .\scripts\run_boot.ps1 <run name> [seconds] [-EmptyRomfs] [-NoFront]
#   -> build\<run name>\{stdout.txt, stderr.txt, boot_trace.txt, userdata\}; prints the exit code and the key lines.
param(
    [Parameter(Mandatory)][string]$Name,
    [int]$Seconds = 60,
    [switch]$EmptyRomfs,
    [switch]$NoFront
)
$root = Split-Path $PSScriptRoot
$run = Join-Path $root "build\$Name"
New-Item -ItemType Directory -Force "$run\userdata" | Out-Null
if ($EmptyRomfs) { New-Item -ItemType Directory -Force "$run\romfs_empty" | Out-Null; $env:SSBU_ROMFS = "$run\romfs_empty" }
else { $env:SSBU_ROMFS = Join-Path $root 'targets\romfs' }
$env:SSBU_USERDATA = "$run\userdata"
$env:SSBU_TRACE = "$run\boot_trace.txt"
# display.c keeps test runs off-screen and out of the taskbar unless SSBU_SHOW=1: show the window (-NoFront: hidden)
$env:SSBU_SHOW = if ($NoFront) { '0' } else { '1' }
# nnaudio.c mutes test copies the same way: a watched run is heard (-NoFront: silent)
$env:SSBU_VOLUME = if ($NoFront) { '0' } else { '100' }
# nnmisc.c suspends the game (as the console does at HOME) once its window has been in front and then loses the focus;
# a watched test run must keep running while the person watching uses other windows
$env:SSBU_FOCUS_NOTIFY = '0'
$fonts = Join-Path $root 'targets\sysdata\fonts'          # system shared fonts (scripts/fw_fonts.py from our firmware dump)
if (Test-Path $fonts) { $env:SSBU_FONTS = $fonts }

$psi = New-Object Diagnostics.ProcessStartInfo (Join-Path $root 'build\game_native.exe'), "`"$(Join-Path $root 'targets\exefs\main.elf')`" $Seconds"
$psi.WorkingDirectory = Join-Path $root 'build'
$psi.UseShellExecute = $false
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$sw = [Diagnostics.Stopwatch]::StartNew()
$p = [Diagnostics.Process]::Start($psi)
# Bring the game window to the front once it exists, so the run can be watched (-NoFront: the window stays hidden)
if (-not $NoFront) {
    Add-Type -Namespace RunBoot -Name W -MemberDefinition '[DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c); [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);' -ErrorAction SilentlyContinue
    for ($i = 0; $i -lt 100 -and -not $p.HasExited; $i++) {
        $p.Refresh()
        if ($p.MainWindowHandle -ne [IntPtr]::Zero) { [RunBoot.W]::ShowWindow($p.MainWindowHandle, 9) | Out-Null; [RunBoot.W]::SetForegroundWindow($p.MainWindowHandle) | Out-Null; break }
        Start-Sleep -Milliseconds 200
    }
}
$out = $p.StandardOutput.ReadToEndAsync()
$err = $p.StandardError.ReadToEndAsync()
if (-not $p.WaitForExit(($Seconds + 120) * 1000)) { $p.Kill(); 'KILLED (no exit within the limit + 120 s)' }
$p.WaitForExit()
$out.Result | Out-File "$run\stdout.txt" -Encoding utf8
$err.Result | Out-File "$run\stderr.txt" -Encoding utf8
$codes = @{ 0 = 'ok'; -1073741819 = 'ACCESS_VIOLATION'; -1073740940 = 'HEAP_CORRUPTION'; -1073741571 = 'STACK_OVERFLOW'; -1073740791 = 'STACK_BUFFER_OVERRUN / fail-fast'; 3 = 'abort()' }
$what = $codes[$p.ExitCode]
'exit code 0x{0:X8} {1} after {2:N0} s' -f $p.ExitCode, $what, $sw.Elapsed.TotalSeconds
Select-String -Path "$run\stdout.txt" -Pattern '\[phase|SetMemoryHeapSize|AllocateMemoryBlock|HOST EXCEPTION|reading address|writing address|last guest function|guest call stack|still running|nnMain returned|trap|TRAP|imports reached|without a handler' |
    Select-Object -First 40 | ForEach-Object { $_.Line.Substring(0, [Math]::Min(220, $_.Line.Length)) }
