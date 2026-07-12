#!/usr/bin/env bash
# Format first-party sources, build, and run the tests (Linux/macOS).
# Usage: scripts/check.sh
set -euo pipefail

cd "$(dirname "$0")/.."

command -v clang-format >/dev/null || {
	echo "clang-format not found on PATH. Install it (apt install clang-format / brew install clang-format)." >&2
	exit 1
}

echo "==> clang-format (src, tests)"
find src tests -type f \( -name '*.cpp' -o -name '*.h' -o -name '*.hpp' \) \
	-print0 | xargs -0 clang-format -i

echo "==> configure + build"
cmake --preset=default
cmake --build build

echo "==> test"
ctest --preset=default
