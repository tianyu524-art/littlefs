$ErrorActionPreference = "Stop"

$root = "C:\Users\HW\.codex\worktrees\e439\littlefs_v0"
$exe = Join-Path $root "autotest\pwr_autotest.exe"

Push-Location $root
try {
    gcc -I. -I./bd -std=c99 -Wall -Wextra -pedantic `
        -o autotest/pwr_autotest.exe `
        autotest/pwr_autotest.c `
        lfs.c `
        lfs_util.c `
        bd/lfs_filebd.c
}
finally {
    Pop-Location
}

Write-Host "Built $exe"
