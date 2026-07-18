@echo off
setlocal

where gcc >nul 2>nul
if errorlevel 1 (
  echo gcc not found in PATH. Install MinGW-w64 or run from an MSYS2 shell.
  exit /b 1
)

where windres >nul 2>nul
if errorlevel 1 (
  echo windres not found in PATH. Install MinGW-w64 or run from an MSYS2 shell.
  exit /b 1
)

windres appicon.rc -O coff -o appicon.res
if errorlevel 1 (
  echo Resource compilation failed.
  exit /b 1
)

gcc -std=c11 -Os -s -municode -mwindows notepad.c appicon.res -o simple_notepad.exe -lcomdlg32 -lshell32
if errorlevel 1 (
  echo Build failed.
  exit /b 1
)

echo Build succeeded: simple_notepad.exe
endlocal
