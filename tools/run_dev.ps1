# Builds nova.exe and runs it once against the local dev data (muted, windowed, auto-exit), then prints the log tail.
#   pwsh tools\run_dev.ps1 [-Seconds 45] [-ShotEvery 10] [-Build] [-Extra "--trace"] [-Tail 60]
param([int]$Seconds = 45, [double]$ShotEvery = 10, [switch]$Build, [string]$Extra = "", [string]$Script = "", [int]$Tail = 60)
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root
if ($Build) {
  cmd /c "tools\vsenv.cmd cmake --build --preset win-release --target nova 2>&1" | Select-String -Pattern "error|FAILED" | Select-Object -First 30
}
New-Item -ItemType Directory -Force build\dev\shots | Out-Null
$args = "--data game\run --seed game\data\sdcard_Android_data\com.gameloft.android.ANMP.GloftNOHM\files --mute " +
        "--shots build\dev\shots --shot-every $ShotEvery --exit-after $Seconds $Extra"
if ($Script) { $args += " --script `"$Script`"" }
$p = Start-Process -FilePath build\win-release\src\nova.exe -ArgumentList $args -PassThru -NoNewWindow `
       -RedirectStandardError build\dev\run_stderr.txt -RedirectStandardOutput build\dev\run_stdout.txt
if (!$p.WaitForExit(($Seconds + 30) * 1000)) { Stop-Process -Id $p.Id -Force; "killed (timeout)" }
"exit $($p.ExitCode)"
Get-Content build\win-release\src\nova.log | Select-Object -Last $Tail
