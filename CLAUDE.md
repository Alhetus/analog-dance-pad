# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Big picture

Analog Dance Pad is software for FSR-based dance-game pads built on ATmega32u4 / Teensy 2.0 boards.

- `firmware/` contains the firmware for the ADP boards.
- `adp-tool/` (C++/CMake) is the new "ADP Server". It talks to devices over hidapi and exposes a **plain WebSocket JSON server on `ws://127.0.0.1:8008`**.

Clone note: this repo uses git submodules (under `adp-tool/lib`, `adp-tool/vcpkg`). Clone with `--recurse-submodules`.

## adp-tool (C++, server)

Threading model (`src/Main.cpp`): two threads communicating through a blocking queue (`MSGQ.hpp`):
- **Device-I/O thread** (`Application::UpdateLoop`) — ~60 Hz loop and the *sole owner of all hidapi access*. Each tick it runs `Device::Update()` (discover → poll sensors → build and atomically publish an immutable `SensorSnapshot`), broadcasts the latest snapshot JSON (`Device::SnapshotToJson`) to all WebSocket clients, then drains the inbound queue and applies each message via `Device::HandleClientMessage`. Because every device touch happens here, there are no data races on device state and no concurrent HID calls.
- **WebSocket thread** (`WebsocketServer::Init`) — ixwebsocket server on port 8008. Inbound client messages are pushed by value onto the queue; the device-I/O thread drains and dispatches them. The server is owned by `WebsocketServer` (no dangling pointers) and stopped via `WebsocketServer::Stop()`.

Sensor state crosses threads only as `std::atomic<std::shared_ptr<const SensorSnapshot>>` (`Device::GetSnapshot()`), so readers are lock-free and never see torn/partial state.

Shutdown is graceful: `SIGINT`/`SIGTERM` clears an atomic flag, the loop exits, the server is stopped, the queue is drained, threads join, and destructors run (`Device::Shutdown` saves/closes the device).

Device access lives in `src/Model/` (`Device`, `Reporter`, `Firmware`; wire-format helpers in `Wire.h`). This is a console app (`/SUBSYSTEM:Console`).

Build (from `adp-tool/`), dependencies via vcpkg submodule. Presets live in `CMakePresets.json`:
```
cmake --preset=default        # configure (RelWithDebInfo, tests + warnings-as-errors on)
cmake --build build
ctest --preset=default        # run the unit tests (tests/)
```
Sanitizer presets (Linux/Clang/GCC): `asan-ubsan` and `tsan`. C++20, static triplet `x64-windows-static-md` on Windows. Unit tests use Catch2 (`tests/`).
