# Building the Installer

opennote uses [Inno Setup](https://jrsoftware.org/isinfo.php) to create the Windows installer.

## Prerequisites

1. Install [Inno Setup](https://jrsoftware.org/isdl.php) (free)
2. Build opennote first:
   ```bash
   cmake -B build -G "Visual Studio 17 2022" -A x64
   cmake --build build --config Release
   ```

## Building the Installer

### Option 1: Using Inno Setup GUI

1. Open `installer/OpenNoteSetup.iss` in Inno Setup
2. Click Build > Compile
3. The installer will be created at `build/installer/OpenNoteSetup.exe`

### Option 2: Command Line

```bash
# From the project root
"C:\Program Files (x86)\Inno Setup 6\ISCC.exe" installer\OpenNoteSetup.iss
```

## Installer Features

- Installs to `C:\Program Files\opennote` by default
- Optional desktop shortcut
- Optional Quick Launch shortcut
- Start Menu shortcuts
- "Open with opennote" context menu entry
- Clean uninstaller

## Notes

- The installer requires the `.ico` icon file at `gfx/icon.ico`
- Convert the PNG icon to ICO format if needed using an online converter or ImageMagick:
  ```bash
  magick convert gfx/icon.png -define icon:auto-resize=256,128,64,48,32,16 gfx/icon.ico
  ```
