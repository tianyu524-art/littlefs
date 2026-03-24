@echo off
setlocal

cd /d "%~dp0"

where gcc >nul 2>nul
if errorlevel 1 (
    echo gcc not found in PATH.
    exit /b 1
)

gcc -I. -I.\bd -std=c99 -Wall -Wextra -pedantic ^
    -o simulator\littlefs_simulator.exe ^
    lfs.c lfs_util.c bd\lfs_rambd.c bd\lfs_filebd.c simulator\littlefs_simulator.c

if errorlevel 1 (
    echo Build failed.
    exit /b 1
)

echo Build succeeded: simulator\littlefs_simulator.exe
exit /b 0
