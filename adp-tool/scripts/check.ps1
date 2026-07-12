# Format first-party sources, build, and run the tests (Windows).
# Usage: pwsh scripts/check.ps1   (or  powershell -File scripts\check.ps1)
$ErrorActionPreference = 'Stop'

Set-Location (Join-Path $PSScriptRoot '..')

if (-not (Get-Command clang-format -ErrorAction SilentlyContinue)) {
	Write-Error 'clang-format not found on PATH. Install it: winget install LLVM.LLVM'
	exit 1
}

Write-Host '==> clang-format (src, tests)'
Get-ChildItem -Recurse -Path src, tests -Include *.cpp, *.h, *.hpp -File |
	ForEach-Object { clang-format -i $_.FullName }

Write-Host '==> configure + build'
cmake --preset=windows
if ($LASTEXITCODE) { exit $LASTEXITCODE }
cmake --build build --config RelWithDebInfo
if ($LASTEXITCODE) { exit $LASTEXITCODE }

Write-Host '==> test'
ctest --preset=windows
if ($LASTEXITCODE) { exit $LASTEXITCODE }
