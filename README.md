# Simple Notepad

A small Windows notepad-style text editor written in C with the Win32 API.

## Features

- Create, open, save, and save-as text files
- Edit actions: undo, cut, copy, paste, select all
- Unsaved-changes prompt before closing/opening/new file
- Text size controls (menu and shortcuts)
- UTF-8 save support and UTF-8/ANSI load handling
- Optional file path argument on launch

## Project Files

- `notepad.c` - Application source code
- `build.bat` - Build script that compiles the app with GCC

## Requirements

- Windows
- GCC in `PATH` (for example via MinGW-w64 or MSYS2)

## Build

Run this in the project folder:

```bat
build.bat
```

On success, the build creates:

- `simple_notepad.exe`

## Run

Start the app:

```bat
simple_notepad.exe
```

Open a file directly:

```bat
simple_notepad.exe C:\path\to\file.txt
```

## Notes

- The build uses `-mwindows`, so the app starts as a GUI program (no console window).
- Saved files are written as UTF-8 without BOM.
