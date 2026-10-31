param([string]$Compiler = 'g++')
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$build=Join-Path $root 'build-host-tests'
New-Item -ItemType Directory -Path $build -Force | Out-Null
$exe=Join-Path $build 'mapping-tests.exe'
$compilerPath=(Get-Command $Compiler -ErrorAction Stop).Source
$originalPath=$env:PATH
try {
    $env:PATH=(Split-Path -Parent $compilerPath)+';'+$originalPath
    & $compilerPath -std=c++17 -Wall -Wextra -Werror "-I$root/main" "-I$PSScriptRoot/mapping_host_stubs" (Join-Path $PSScriptRoot 'test_mapping.cpp') (Join-Path $root 'main/ns2_mapping_core.cpp') (Join-Path $root 'main/ns2_mapping.cpp') -o $exe
    if($LASTEXITCODE -ne 0){throw 'Mapping host test compile failed'}
    & $exe
    if($LASTEXITCODE -ne 0){throw 'Mapping host tests failed'}
} finally { $env:PATH=$originalPath }
