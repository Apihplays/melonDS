# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

melonDS is a Nintendo DS and DSi emulator written in C++17 and C11, featuring both an interpreter and JIT recompilation (x86_64 and ARM64), 2D/3D GPU pipelines (software and OpenGL renderers), audio emulation, Wi-Fi networking, and a Qt6/SDL frontend.

## Build Commands

Build system is CMake (>= 3.16) and supports presets via `CMakePresets.json`.

### Local Build (WSL2 / Arch / Ubuntu)
```bash
# Configure release build
cmake -B build -DCMAKE_BUILD_TYPE=Release

# Build executable (output placed at build/melonDS)
cmake --build build -j$(nproc)

# Using CMake presets
cmake --preset release
cmake --build --preset release

# Run directly
./build/melonDS [path/to/game.nds]
```

### Key CMake Options
- `-DENABLE_JIT=ON|OFF`: JIT recompiler (x86_64 / ARM64). Default: `ON`.
- `-DENABLE_OGLRENDERER=ON|OFF`: OpenGL 3D hardware renderer. Default: `ON`.
- `-DBUILD_QT_SDL=ON|OFF`: Build Qt/SDL frontend. Default: `ON`.
- `-DUSE_QT6=ON|OFF`: Qt version (default is `ON` for Qt6; set `OFF` for Qt5).
- `-DENABLE_GOOGLE_DRIVE_SYNC=ON|OFF`: Google Drive save sync. Default: `ON`.
- `-DENABLE_GDBSTUB=ON|OFF`: GDB remote debugging stub. Default: `ON`.
- `-DUSE_VCPKG=ON|OFF`: Enable vcpkg dependency management. Default: `OFF`.

### Testing & Validation
There is no automated unit test suite for the emulation core. Validation is performed by compiling cleanly, checking for regressions, and executing test ROMs or inspecting debug logs.

## CI & Release Automation

- **Branch**: `master` is the main development branch.
- **Release Triggers**: Pushing tags matching `v*` (e.g. `v1.1.1`) triggers the automated release jobs in `.github/workflows/build-windows.yml` and `build-ubuntu.yml`.
- **Release Artifacts**:
  - Windows: standalone `melonDS-windows-x86_64.zip`
  - Linux: portable `melonDS-linux-x86_64.AppImage`
- **GitHub Actions Requirement**: Always use `actions/upload-artifact@v4` and `actions/download-artifact@v4`. Do not reference non-existent major versions like `v7`.

## Architecture & Code Structure

The project separates the emulation core (`src/`, static library `core`) from the UI frontend (`src/frontend/qt_sdl/`).

### Core Emulation (`src/`)
- **System Controller (`NDS.cpp` / `DSi.cpp`)**: Coordinates main execution loop, memory bus routing, interrupt controllers, and subsystem timing synchronization.
- **CPU & Execution**:
  - `ARMInterpreter*.cpp`: Interprets ARM7TDMI (33 MHz) and ARM946E-S (66 MHz) instruction sets.
  - `ARMJIT_x64/` & `ARMJIT_A64/`: JIT recompilers for x86_64 and AArch64.
  - `CP15.cpp`: System control coprocessor (MMU, MPU, cache, tightly-coupled memories).
- **Graphics Subsystem**:
  - `GPU.cpp` & `GPU2D.cpp` / `GPU2D_Soft.cpp`: 2D display engines A and B, tile rendering, affine layers, compositing.
  - `GPU3D.cpp` / `GPU3D_Soft.cpp`: Geometry engine and software rasterizer (fixed-point math, polygon clipping).
  - `GPU_OpenGL.cpp` & `OpenGL_shaders/`: Hardware OpenGL renderer.
- **Audio Subsystem**:
  - `SPU.cpp`: Sound processing unit (16 channels: PCM, ADPCM, PSG).
  - `teakra/` & `DSP_HLE/`: DSi Teakra DSP emulation (LLE and HLE).
  - `blip-buf/`: Band-limited sound synthesis buffer.
- **Peripherals & I/O**:
  - `NDSCart/`: Game card bus, EEPROM, FLASH, NAND, and homebrew SD interfaces.
  - `DMA.cpp` / `DSi_NDMA.cpp`: Direct Memory Access controllers and hardware timings.
  - `SPI.cpp` / `SPI_Firmware.cpp` / `FreeBIOS.cpp`: Serial Peripheral Interface, firmware NVRAM, and embedded FreeBIOS fallback.
  - `Wifi.cpp` / `WifiAP.cpp` / `net/`: Wi-Fi MAC emulation, packet capture (libpcap), and user-mode NAT network stack (libslirp).
  - `Savestate.cpp`: Core state serialization and snapshotting.

### Frontend (`src/frontend/qt_sdl/`)
- Qt-based GUI (`main.cpp`, `MainWindow.cpp`, settings dialogs).
- SDL2-based audio output and gamepad input handling.
- `CloudSyncManager.cpp`: Google Drive OAuth 2.0 PKCE save synchronization (`cloudsync.json`).
- `Config.cpp`: Loads and saves settings to `melonDS.toml`.

## Code Style & Conventions

Follow `CONTRIBUTING.md` guidelines:
- **Indentation**: 4 spaces, no tabs.
- **Braces**: Opening brace on a new line for functions, classes, and namespaces.
- **Naming**:
  - Namespaces: `PascalCase` (content not indented).
  - Classes & Structs: `PascalCase`.
  - Functions & Methods: `PascalCase`.
  - Local variables & Function parameters: `camelCase`.
  - Class Members: `PascalCase` (no prefix, no `m_`).
  - Preprocessor defines & macros: `CAPITALISED_SNAKE_CASE`.
  - Enums: `enumPrefix_ItemName` (camelCase prefix + PascalCase item name).
- **Headers**:
  - Use C standard library C headers directly (`#include <stdio.h>`, not `<cstdio>`).
  - Keep variable declarations close to their first use with minimal scope.
- **Git Commits**: Author `Apihplays <apihplays@example.com>`. Include `Co-Authored-By: Claude Code <noreply@anthropic.com>`.
