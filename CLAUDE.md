# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Big picture

Analog Dance Pad is software for FSR-based dance-game pads built on ATmega32u4 / Teensy 2.0 boards.

- `firmware/` contains the firmware for the ADP boards.
- `adp-tool/` (C++/CMake) is the new "ADP Server". It talks to devices over hidapi and exposes a **plain WebSocket JSON server on `ws://127.0.0.1:8008`** — **open to any client by design** (trusted venue LAN and users, no authentication). Don't flag that in reviews; the optional `--allowed-origin` / `--token` flags exist but are off by default. See README "Security model".

Clone note: this repo uses git submodules (under `adp-tool/lib`, `adp-tool/vcpkg`). Clone with `--recurse-submodules`.

## adp-tool (C++, server)

Threading model (`src/Main.cpp`): device-I/O and WebSocket threads communicating through a blocking queue (`MSGQ.hpp`), plus a player scanner:
- **Device-I/O thread** (`Application::UpdateLoop`) — ~60 Hz loop that owns hidapi access, except for two kinds of short-lived worker (`std::async`) that each own *one* pad's handle while they run: a pad's EEPROM save (`PadDevice::StartSave`, can take 1s+; the pad is skipped and its messages held until `FinishSave` collects it, snapshots carry `saving: true`) and a new pad's connect stage 2 (`BuildPad`, ~90 transfers). `hid_enumerate` runs ~1Hz on a third kind of worker (no handle; opening a pad never overlaps it), `hid_open_path` stays on this thread. All connected pads are polled every tick: `Device::Update()` (collect finished background work → poll every pad → start due saves → publish one immutable `SensorSnapshot` per pad), publishes the pad list on change, sends each snapshot (`Device::SnapshotToJson`, tagged with the pad's HID path) to the clients subscribed to that pad (`{subscribe: "*"|<path>|""}`, default `*`), publishes players and lights on change only (`WebsocketServer::Publish` replays the latest copy to each new client), then drains the inbound queue via `Device::HandleClientMessage`. Every device message carries `pad: <path>`; `HandleClientMessage` looks the pad up and calls it directly, and answers profile/device ops with a msgType 8 result to the sender (echoing its `requestId`, if any). Sensor/lights edits are applied at most every 50ms per pad (`Application::CONFIGURE_INTERVAL`), with queued ones of a pad folded together whichever client sent them (every folded sender is answered on failure; a success only to senders with a `requestId`). Device state is only touched on this thread, and no two threads ever use the same HID handle.
- **WebSocket thread** (`WebsocketServer::Init`) — ixwebsocket server on port 8008. Inbound client messages are pushed by value onto the queue; the device-I/O thread drains and dispatches them. `getAvatar` and `subscribe` are handled directly on this thread (file-only, can be MBs). The server is owned by `WebsocketServer` (no dangling pointers) and stopped via `WebsocketServer::Stop()`.
- **Player scanner** (`Model/Players.cpp`) — rescans `--local-profiles-dir` (ITGmania LocalProfiles, `DisplayName=` in `Editable.ini`) every 30s or on `rescanPlayers` (at most every 5s), follows player renames, and publishes an immutable player list.

Profiles live on each server's local disk at `<--profiles-dir>/players/<player>/<pad name>/<id>.json` (max 5 per player per pad); per-pad selection at `pads/<pad name>/state.json`. Only LocalProfiles is on a network share; while it is unreachable the last known player list is kept and rescans run every 5s. Run with `--host 0.0.0.0` so phones/tablets can connect.

Sensor state crosses threads only as an immutable `shared_ptr<const vector<SensorSnapshot>>` (`Device::GetSnapshots()`), swapped under a short mutex (just a pointer copy), so readers never see torn/partial state.

Shutdown is graceful: `SIGINT`/`SIGTERM` clears an atomic flag, the loop exits, the server is stopped, threads join, and destructors run (`Device::Shutdown` saves/closes the device).

Device access lives in `src/Model/` (`Device`, `PadDevice`, `Reporter`; wire-format helpers in `Wire.h`). Firmware older than v1.3 (no IdentificationV2 report) is not supported. This is a console app (`/SUBSYSTEM:Console`).

Build (from `adp-tool/`), dependencies via vcpkg submodule. Presets live in `CMakePresets.json`:
```
cmake --preset=default        # configure (RelWithDebInfo, tests + warnings-as-errors on)
cmake --build build
ctest --preset=default        # run the unit tests (tests/)
```
Sanitizer presets (Linux/Clang/GCC): `asan-ubsan` and `tsan`. C++20, static triplet `x64-windows-static-md` on Windows. Unit tests use Catch2 (`tests/`).
