[CmdletBinding()]
param([string]$Compiler, [string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
if (-not $Compiler) {
    $candidate = Join-Path $env:LOCALAPPDATA 'Programs/CLion/bin/mingw/bin/g++.exe'
    if (Test-Path -LiteralPath $candidate) { $Compiler = $candidate }
    else { $Compiler = (Get-Command g++ -ErrorAction Stop).Source }
}
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $root '.cache/host-tests' }
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$out = Join-Path $OutputDirectory 'test-input.exe'
& $Compiler -std=c++17 -Wall -Wextra -Werror -DNS2_HOST_TEST '-static-libgcc' '-static-libstdc++' "-I$root/main" "-I$PSScriptRoot/tests/input-stubs" "$root/main/ns2_input.cpp" "$root/main/ns2_gatt.cpp" "$root/main/ns2_protocol.cpp" "$PSScriptRoot/tests/test_input.cpp" -o $out
if ($LASTEXITCODE -ne 0) { throw 'Input protocol host test compile failed' }
$oldPath = $env:PATH
try {
    $env:PATH = "$(Split-Path $Compiler -Parent);$oldPath"
    & $out
    if ($LASTEXITCODE -ne 0) { throw "Input protocol test failed: $LASTEXITCODE" }
} finally { $env:PATH = $oldPath }
