# Analog Dance Pad

Software for FSR-based dance-game pads built on ATmega32u4 / Teensy 2.0 boards.

- **`firmware/`** — firmware for the ADP boards (PlatformIO).
- **`adp-tool/`** — the ADP Server (C++/CMake). Talks to devices over hidapi and
  exposes a plain WebSocket JSON server on `ws://127.0.0.1:8008`.

This document covers building and testing **`adp-tool`**.

## Clone

The repo uses git submodules (vcpkg and vendored libraries under `adp-tool/`).
Clone recursively:

```
git clone --recurse-submodules https://github.com/Alhetus/analog-dance-pad
```

Already cloned without `--recurse-submodules`? Pull the submodules in:

```
git submodule update --init --recursive
```

## Prerequisites

You need **CMake ≥ 3.21** and a **C++20 compiler**. Dependencies (fmt,
nlohmann-json, argparse, and Catch2 for the tests) are fetched and
built automatically by the bundled **vcpkg** submodule — you do **not** need to
install vcpkg separately or set `VCPKG_ROOT`.

> The first configure compiles all vcpkg dependencies from source, so it can
> take several minutes. Subsequent configures are cached.

**Windows** — Visual Studio 2022 with the "Desktop development with C++"
workload, plus CMake (`choco install cmake`, or bundled with Visual Studio).

**Linux** (Debian/Ubuntu):

```
sudo apt update && sudo apt install -y build-essential cmake ninja-build libudev-dev
```

**macOS** — Xcode Command Line Tools (`xcode-select --install`) plus CMake:

```
brew install cmake pkg-config
```

## Build

All commands run from the `adp-tool/` directory. The build is driven by
`CMakePresets.json`; the `default` preset configures a `RelWithDebInfo` build
with the unit tests and warnings-as-errors on. Windows uses the `windows`
preset, which is `default` plus the `x64-windows-static-md` vcpkg triplet.

**Windows:**

```
cd adp-tool
cmake --preset=windows
cmake --build build --config RelWithDebInfo
```

**Linux / macOS:**

```
cd adp-tool
cmake --preset=default
cmake --build build
```

The server binary lands at `adp-tool/build/adp-tool` (`adp-tool.exe` on Windows).

## Venue setup (Ubuntu Studio 24.04 cabs)

Each cab runs one server for its pads. The player list comes from ITGmania's
`LocalProfiles` on a network share; the pad profiles are stored locally on each
cab (`--profiles-dir`, default in the unit: `~/.local/share/adp-tool/profiles`),
so a pad's profiles live on the cab it is plugged into.

```
# server + pad access without root
sudo install -m755 adp-tool/build/adp-tool /usr/bin/
sudo cp adp-tool/cmake/deb/99_adp.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules && sudo udevadm trigger

# mount the LocalProfiles share (soft + timeouts: an unreachable share fails fast instead of hanging)
sudo apt install -y cifs-utils && sudo mkdir -p /mnt/adp-share
echo '//fileserver/itg /mnt/adp-share cifs credentials=/etc/adp-share.cred,uid=1000,gid=1000,iocharset=utf8,soft,_netdev,nofail,x-systemd.automount,x-systemd.mount-timeout=10 0 0' | sudo tee -a /etc/fstab

# run at login as the cab user (edit the paths in the unit first)
mkdir -p ~/.config/systemd/user && cp adp-tool/cmake/deb/adp-server.service ~/.config/systemd/user/
systemctl --user daemon-reload && systemctl --user enable --now adp-server
journalctl --user -u adp-server -f   # logs
```

- `iocharset=utf8` keeps names like "Åsa" intact; `uid`/`gid` must be the cab user.
- Back up `~/.local/share/adp-tool/profiles` if the profiles matter: it is the only copy.
- If `ufw` is enabled: `sudo ufw allow 8008/tcp` so phones/tablets can connect.
- While the share is unmounted everything keeps working with the last known
  player list, and it refreshes automatically within a few seconds of the share
  coming back.

### Security model

The server is open by design: any client that can reach port 8008 may connect,
drive the pads, and create, edit or delete profiles. It runs on a trusted venue
LAN for trusted users, so there is no authentication, and the shipped systemd
unit uses none. This is a deliberate choice, not an oversight.

`--allowed-origin <origin>` (repeatable) and `--token <secret>` (clients then
connect with `?token=<secret>`) exist if a venue ever wants to restrict access.

## Test

Unit tests use Catch2 and run through CTest:

```
cd adp-tool
ctest --preset=default    # ctest --preset=windows on Windows
```

## Run

Launch the built binary; it serves the WebSocket JSON API on
`ws://127.0.0.1:8008`:

```
./build/adp-tool
```

## Formatting

First-party sources under `adp-tool/src` and `adp-tool/tests` are formatted with
**clang-format** (config in `adp-tool/.clang-format`). CI rejects unformatted
code, so run it before pushing. Vendored code under `lib/` and `vcpkg/` is not
formatted.

Install clang-format:

- **Windows** — `winget install LLVM.LLVM` (or `choco install llvm`). It also
  ships with the Visual Studio "Desktop development with C++" workload. `winget`
  installs to `C:\Program Files\LLVM\bin`, which is not on `PATH` by default —
  add it (System → Environment Variables, or `[Environment]::SetEnvironmentVariable('Path', "$env:Path;C:\Program Files\LLVM\bin", 'User')`)
  and open a new terminal so `clang-format` resolves.
- **Linux** (Debian/Ubuntu) — `sudo apt install -y clang-format`
- **macOS** — `brew install clang-format`

Format in place:

```
cd adp-tool
find src tests -type f \( -name '*.cpp' -o -name '*.h' -o -name '*.hpp' \) | xargs clang-format -i
```

To check without modifying files (what CI does), swap `-i` for `--dry-run --Werror`.

## Helper scripts

`adp-tool/scripts` has one-shot scripts that format, configure, build, and run
the tests in sequence:

```
cd adp-tool
./scripts/check.sh                 # Linux / macOS
powershell -File scripts\check.ps1 # Windows
```
