# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Big picture

Analog Dance Pad is software for FSR-based dance-game pads built on ATmega32u4 / Teensy 2.0 boards.

- `firmware/` contains the firmware for the ADP boards.
- `adp-tool/` (C++/CMake) is the new "ADP Server". It talks to devices over hidapi and exposes a **plain WebSocket JSON server on `ws://127.0.0.1:8008`**.

Clone note: this repo uses git submodules (under `adp-tool/lib`, `adp-tool/vcpkg`). Clone with `--recurse-submodules`.

## adp-tool (C++, server)

Threading model (`src/Main.cpp`): two threads communicating through a mutex-guarded queue (`MSGQ.hpp`):
- **Application thread** (`Application::UpdateLoop`) — ~60 Hz loop. Each tick calls `Device::Update()` / `DiscoverNewDevices()`, then serializes sensor state (`Device::GetAllSensorStatesAsJson`) and broadcasts it to all WebSocket clients.
- **WebSocket thread** (`WebsocketServer::Init`) — ixwebsocket server on port 8008. Inbound client messages are pushed onto the queue as `QueueMessage`; the Application thread drains the queue each tick. (Inbound handling is currently just logged — this is the extension point for client→server commands.)

Device access lives in `src/Model/` (`Device`, `Reporter`, `Firmware`). This is a console app (`/SUBSYSTEM:Console`).

Build (Windows, from `adp-tool/`), dependencies via vcpkg submodule:
```
set VCPKG_ROOT=vcpkg
set PATH=%VCPKG_ROOT%;%PATH%
cmake --preset=default
cmake --build build
```
C++20, static triplet `x64-windows-static-md` on Windows.
