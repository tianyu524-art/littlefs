$ErrorActionPreference = "Stop"

$root = "C:\Users\HW\.codex\worktrees\e439\littlefs_v0"
$exe = Join-Path $root "autotest\pwr_autotest.exe"
$source = Join-Path $root "simulator\test001.img"
$work = Join-Path $root "autotest\test001_work.img"
$iterations = if ($args.Length -gt 0) { [int]$args[0] } else { 1000 }

if (-not (Test-Path $exe)) {
    & (Join-Path $root "autotest\build_autotest.ps1")
}

& $exe $source $work $iterations
Write-Host "Log written to $root\autotest\autotest.txt"
