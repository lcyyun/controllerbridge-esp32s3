[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [ValidateSet('build', 'flash', 'build-flash', 'monitor', 'devices', 'status', 'mode', 'reboot', 'xinput-test')]
    [string]$Action = 'devices',

    [string]$Port = 'COM12',

    [ValidateSet('nintendo', 'xinput', 'dualsense')]
    [string]$Mode,

    [ValidateSet('n4', 'n8', 'n8r2', 'n8r8', 'n16r8', 'esp32supermini')]
    [string]$Board = 'n16r8',

    [string]$BuildDir
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$ProjectDir = $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($BuildDir)) { $BuildDir = "build-$Board" }
$BuildPath = if ([IO.Path]::IsPathRooted($BuildDir)) {
    [IO.Path]::GetFullPath($BuildDir)
} else {
    [IO.Path]::GetFullPath((Join-Path $ProjectDir $BuildDir))
}
$IdfPath = if ($env:IDF_PATH) { $env:IDF_PATH } else { 'C:\Espressif\frameworks\esp-idf-v5.3.3' }
$ToolsPath = if ($env:IDF_TOOLS_PATH) { $env:IDF_TOOLS_PATH } else { Join-Path $env:USERPROFILE '.espressif' }
$PythonEnv = if ($env:IDF_PYTHON_ENV_PATH) {
    $env:IDF_PYTHON_ENV_PATH
} else {
    Join-Path $ToolsPath 'python_env\idf5.3_py3.12_env'
}
$Python = Join-Path $PythonEnv 'Scripts\python.exe'
$IdfPy = Join-Path $IdfPath 'tools\idf.py'
$Manager = Join-Path $ProjectDir 'ns2-s3-manager.py'

function Initialize-Idf {
    if (-not (Test-Path $Python)) { throw "ESP-IDF Python not found: $Python" }
    if (-not (Test-Path $IdfPy)) { throw "ESP-IDF not found: $IdfPath" }

    $env:IDF_PATH = $IdfPath
    $env:IDF_TOOLS_PATH = $ToolsPath
    $env:IDF_PYTHON_ENV_PATH = $PythonEnv
    $env:PYTHON = $Python
    $env:IDF_COMPONENT_MANAGER = '1'
    # Let this IDF installation select its supported tool versions, not the
    # newest directory name or an unrelated system CMake.
    $exports = & $Python (Join-Path $IdfPath 'tools\idf_tools.py') export --format key-value
    if ($LASTEXITCODE -ne 0) { throw "ESP-IDF tool export failed with exit code $LASTEXITCODE" }
    foreach ($line in $exports) {
        $pair = $line -split '=', 2
        if ($pair.Count -ne 2 -or $pair[0] -notmatch '^[A-Za-z_][A-Za-z0-9_]*$') { continue }
        $value = $pair[1]
        if ($pair[0] -eq 'PATH') { $value = $value.Replace('%PATH%', $env:Path) }
        [Environment]::SetEnvironmentVariable($pair[0], $value, 'Process')
    }
}

function Invoke-Idf([string[]]$Arguments) {
    Initialize-Idf
    & $Python $IdfPy -C $ProjectDir -B $BuildPath "-DNS2_ESP32S3_BOARD=$Board" @Arguments
    if ($LASTEXITCODE -ne 0) { throw "idf.py failed with exit code $LASTEXITCODE" }
}

function Invoke-Manager([string[]]$Arguments) {
    Initialize-Idf
    & $Python $Manager @Arguments
    if ($LASTEXITCODE -ne 0) { throw "manager command failed with exit code $LASTEXITCODE" }
}

switch ($Action) {
    'build'       { Invoke-Idf @('build') }
    'flash'       { Invoke-Idf @('-p', $Port, '-b', '460800', 'flash') }
    'build-flash' { Invoke-Idf @('-p', $Port, '-b', '460800', 'build', 'flash') }
    'monitor'     { Invoke-Idf @('-p', $Port, 'monitor') }
    'status'      { Invoke-Manager @('status') }
    'reboot'      { Invoke-Manager @('reboot') }
    'xinput-test' { Invoke-Manager @('xinput-test') }
    'mode' {
        if (-not $Mode) { throw 'mode action requires -Mode nintendo|xinput|dualsense' }
        Invoke-Manager @('mode', $Mode, '--reboot')
    }
    'devices' {
        Get-PnpDevice -PresentOnly |
            Where-Object {
                $_.InstanceId -match 'VID_(057E&PID_2069|1209&PID_4E53|054C&PID_0CE6)' -or
                $_.FriendlyName -match 'NS2Pro|Xbox 360 Controller|DualSense'
            } |
            Select-Object Status, Class, FriendlyName, InstanceId, Problem |
            Format-Table -AutoSize
    }
}
