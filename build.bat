@echo off
setlocal

where gcc >nul 2>nul
if errorlevel 1 (
  echo gcc not found in PATH. Install MinGW-w64 or run from an MSYS2 shell.
  exit /b 1
)

gcc -std=c11 -Os -s -municode -mwindows notepad.c -o simple_notepad.exe -lcomdlg32 -lshell32
if errorlevel 1 (
  echo Build failed.
  exit /b 1
)

echo Build succeeded: simple_notepad.exe
endlocal
