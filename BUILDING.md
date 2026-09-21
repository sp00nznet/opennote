# Building opennote

## Requirements

- **Windows 10/11**
- **Visual Studio 2022** (or compatible MSVC toolchain)
- **CMake 3.16+**

## Build Steps

### Command Line (Recommended)

```bash
# Clone the repository
git clone https://github.com/sp00nznet/opennote.git
cd opennote

# Configure with CMake
cmake -B build

# Build Release version
cmake --build build --config Release

# Build Debug version (for development)
cmake --build build --config Debug
```

The executable will be at `build/bin/Release/OpenNote.exe` or `build/bin/Debug/OpenNote.exe`.

### Visual Studio

1. Open Visual Studio 2022
2. Select "Open a local folder" and choose the opennote directory
3. CMake will automatically configure the project
4. Select your build configuration (Debug/Release)
5. Build > Build All (Ctrl+Shift+B)

### Developer Command Prompt

```bash
# Open "x64 Native Tools Command Prompt for VS 2022"
cd path\to\opennote
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

## Dependencies

All dependencies are included in the repository:

| Dependency | Location | Purpose |
|------------|----------|---------|
| SQLite 3 | `lib/sqlite/` | Database storage |
| Windows SDK | System | Common controls, dialogs |

## Build Options

The CMakeLists.txt supports the following options:

```cmake
# Example: Enable additional warnings
cmake -B build -DCMAKE_C_FLAGS="/W4"
```

## Troubleshooting

### "Cannot find cmake"
- Install CMake from https://cmake.org/download/
- Add CMake to your PATH during installation

### "Cannot find compiler"
- Install Visual Studio 2022 with "Desktop development with C++" workload
- Use the Developer Command Prompt or ensure MSVC is in PATH

### "Missing Windows SDK"
- Install Windows SDK via Visual Studio Installer
- Or download from https://developer.microsoft.com/windows/downloads/windows-sdk/

### Link errors with SQLite
- Ensure `lib/sqlite/sqlite3.c` is included in the build
- The amalgamation should compile as part of the project

## Clean Build

```bash
# Remove build directory and rebuild
rm -rf build
cmake -B build
cmake --build build --config Release
```

## Running Tests

Currently, opennote does not have automated tests. Manual testing is performed on:
- File operations (new, open, save, save as)
- Tab management (create, close, switch)
- Notes database (create, edit, delete, search)
- Session save/restore
- Print and print preview

## Continuous Integration

See `.gitlab-ci.yml` for the CI/CD pipeline configuration. The pipeline:
1. Builds the project with CMake
2. Packages the executable as an artifact
3. Runs on Windows runners with Visual Studio
