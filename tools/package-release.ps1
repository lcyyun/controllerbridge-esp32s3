#Requires -Version 5.1
[CmdletBinding()]
param(
    [string]$BuildDirectory,

    [ValidateSet('n16r8')]
    [string]$Board = 'n16r8'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    throw 'BuildDirectory must be explicitly supplied; no build directory is selected automatically.'
}
if ($Board -cne 'n16r8') { throw 'Only the n16r8 board is supported.' }

$RepositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$BuildRoot = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($BuildDirectory)
$DistRoot = Join-Path $RepositoryRoot 'dist'
$FlashCapacity = 16MB

function Get-SafeRelativePath([object]$Value) {
    if ($Value -isnot [string] -or [string]::IsNullOrWhiteSpace($Value)) {
        throw 'Flash image paths must be nonempty relative strings.'
    }
    $relative = $Value.Replace('\', '/')
    if ([IO.Path]::IsPathRooted($relative) -or $relative -match '[:\s]') {
        throw "Unsafe relative path: $Value"
    }
    foreach ($part in $relative.Split('/')) {
        if ($part -in @('', '.', '..') -or
            $part -notmatch '^[A-Za-z0-9_.-]+$' -or $part.EndsWith('.') -or
            $part -match '^(CON|PRN|AUX|NUL|COM[0-9]|LPT[0-9])(\.|$)') {
            throw "Unsafe relative path: $Value"
        }
    }
    return $relative
}

function Get-VerifiedItem([string]$Root, [string]$Relative, [switch]$Directory) {
    $rootItem = Get-Item -LiteralPath $Root -Force
    if (-not $rootItem.PSIsContainer -or
        ($rootItem.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw "Expected a regular directory, not a reparse point: $Root"
    }
    $path = [IO.Path]::GetFullPath((Join-Path $Root $Relative))
    $prefix = $Root.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    if (-not $path.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Path escapes its root: $Relative"
    }
    $current = $Root
    foreach ($part in $Relative.Replace('\', '/').Split('/')) {
        $current = Join-Path $current $part
        $item = Get-Item -LiteralPath $current -Force
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw "Reparse points are not allowed: $current"
        }
    }
    if ($Directory.IsPresent -ne [bool]$item.PSIsContainer) {
        throw "Unexpected file/directory type: $path"
    }
    return $item
}

function Get-RequiredProperty([object]$Object, [string]$Name) {
    if ($null -eq $Object -or $Object -isnot [pscustomobject]) {
        throw "Expected a JSON object containing '$Name'."
    }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property -or $null -eq $property.Value) {
        throw "Missing JSON property: $Name"
    }
    return ,$property.Value
}

function Assert-String([object]$Actual, [string]$Expected, [string]$Label) {
    if ($Actual -isnot [string] -or $Actual -cne $Expected) {
        throw "$Label must be '$Expected'."
    }
}

function Get-Offset([object]$Value) {
    if ($Value -isnot [string] -or $Value -notmatch '^0x[0-9a-fA-F]{1,8}$') {
        throw "Invalid flash offset: $Value"
    }
    return [Convert]::ToUInt32($Value.Substring(2), 16)
}

function Assert-FlashOptions([object[]]$Arguments) {
    $expected = @{ '--flash_mode' = 'dio'; '--flash_size' = '16MB'; '--flash_freq' = '80m' }
    $seen = @{}
    if ($Arguments.Count -ne 6) { throw 'Unexpected write_flash arguments.' }
    for ($index = 0; $index -lt $Arguments.Count; $index += 2) {
        $key = $Arguments[$index]
        if ($key -isnot [string] -or $key -cnotin @('--flash_mode', '--flash_size', '--flash_freq') -or
            $seen.ContainsKey($key)) {
            throw 'Unexpected or duplicate write_flash option.'
        }
        Assert-String $Arguments[$index + 1] $expected[$key] $key
        $seen[$key] = $true
    }
}

$cacheFile = Get-VerifiedItem $BuildRoot 'CMakeCache.txt'
$cache = Get-Content -LiteralPath $cacheFile.FullName
foreach ($setting in @{
    NS2_ESP32S3_BOARD = $Board
    IDF_TARGET = 'esp32s3'
    CMAKE_PROJECT_NAME = 'ns2pro_bridge_esp32s3_n16r8'
}.GetEnumerator()) {
    $matchesForKey = @($cache | Select-String -Pattern "^$($setting.Key):[^=]+=(.*)$")
    if ($matchesForKey.Count -ne 1 -or
        $matchesForKey[0].Matches[0].Groups[1].Value -cne $setting.Value) {
        throw "Build board/target proof failed: $($setting.Key) must be '$($setting.Value)'."
    }
}

$jsonFile = Get-VerifiedItem $BuildRoot 'flasher_args.json'
$argsFile = Get-VerifiedItem $BuildRoot 'flash_args'
$flash = Get-Content -LiteralPath $jsonFile.FullName -Raw | ConvertFrom-Json
$extra = Get-RequiredProperty $flash 'extra_esptool_args'
Assert-String (Get-RequiredProperty $extra 'chip') 'esp32s3' 'chip'
Assert-String (Get-RequiredProperty $extra 'before') 'default_reset' 'before'
Assert-String (Get-RequiredProperty $extra 'after') 'hard_reset' 'after'
$stub = Get-RequiredProperty $extra 'stub'
if ($stub -isnot [bool] -or -not $stub) { throw 'Only the standard stub-enabled flash plan is supported.' }
$settings = Get-RequiredProperty $flash 'flash_settings'
Assert-String (Get-RequiredProperty $settings 'flash_mode') 'dio' 'flash_mode'
Assert-String (Get-RequiredProperty $settings 'flash_size') '16MB' 'N16R8 flash_size'
Assert-String (Get-RequiredProperty $settings 'flash_freq') '80m' 'flash_freq'
Assert-FlashOptions (Get-RequiredProperty $flash 'write_flash_args')

$plan = New-Object 'System.Collections.Generic.List[object]'
$destinations = @{}
function Add-Copy([IO.FileInfo]$File, [string]$Relative) {
    if ($destinations.ContainsKey($Relative)) { throw "Duplicate archive path: $Relative" }
    $destinations[$Relative] = $true
    $plan.Add([pscustomobject]@{
        Source = $File.FullName
        Relative = $Relative
        Hash = (Get-FileHash -LiteralPath $File.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    })
}

$files = Get-RequiredProperty $flash 'flash_files'
if ($files -isnot [pscustomobject]) { throw 'flash_files must be an offset-to-path JSON object.' }
$images = @{}
foreach ($property in $files.PSObject.Properties) {
    $offset = Get-Offset $property.Name
    $relative = Get-SafeRelativePath $property.Value
    if (-not $relative.EndsWith('.bin', [StringComparison]::OrdinalIgnoreCase)) {
        throw "Flash image is not a .bin file: $relative"
    }
    if ($images.ContainsKey($offset)) { throw "Duplicate flash offset: $offset" }
    $file = Get-VerifiedItem $BuildRoot $relative
    if ($file.Length -le 0 -or ([long]$offset + $file.Length) -gt $FlashCapacity) {
        throw "Empty image or image outside N16R8 flash capacity: $relative"
    }
    Add-Copy $file $relative
    $images[$offset] = [pscustomobject]@{ Offset = $offset; Relative = $relative; Length = $file.Length }
}
if ($images.Count -lt 3) { throw 'A complete release requires bootloader, partition table, and application images.' }
$end = 0L
foreach ($image in ($images.Values | Sort-Object Offset)) {
    if ($image.Offset -lt $end) { throw "Overlapping flash image: $($image.Relative)" }
    $end = [long]$image.Offset + $image.Length
}
foreach ($required in @{ bootloader = '0x0'; 'partition-table' = '0x8000'; app = '0x10000' }.GetEnumerator()) {
    $entry = Get-RequiredProperty $flash $required.Key
    $offset = Get-Offset (Get-RequiredProperty $entry 'offset')
    $relative = Get-SafeRelativePath (Get-RequiredProperty $entry 'file')
    $encrypted = Get-RequiredProperty $entry 'encrypted'
    if (($encrypted -isnot [bool] -or $encrypted) -and
        ($encrypted -isnot [string] -or $encrypted -cne 'false')) {
        throw "Encrypted flash image is not supported: $($required.Key)"
    }
    if ($offset -ne (Get-Offset $required.Value) -or
        -not $images.ContainsKey($offset) -or $images[$offset].Relative -cne $relative) {
        throw "Missing or inconsistent required image: $($required.Key)"
    }
}
Assert-String (Get-RequiredProperty (Get-RequiredProperty $flash 'app') 'file') `
    'ns2pro_bridge_esp32s3_n16r8.bin' 'N16R8 application image'

# flash_args is shipped verbatim, so validate it independently of the JSON plan.
$tokens = @((Get-Content -LiteralPath $argsFile.FullName -Raw).Trim() -split '\s+')
if ($tokens.Count -ne (6 + 2 * $images.Count)) { throw 'flash_args does not match the complete JSON flash plan.' }
Assert-FlashOptions $tokens[0..5]
$seenOffsets = @{}
for ($index = 6; $index -lt $tokens.Count; $index += 2) {
    $offset = Get-Offset $tokens[$index]
    $relative = Get-SafeRelativePath $tokens[$index + 1]
    if ($seenOffsets.ContainsKey($offset) -or -not $images.ContainsKey($offset) -or
        $images[$offset].Relative -cne $relative) {
        throw 'flash_args has an unsafe, duplicate, or mismatched image reference.'
    }
    $seenOffsets[$offset] = $true
}

Add-Copy $jsonFile 'flasher_args.json'
Add-Copy $argsFile 'flash_args'
foreach ($name in @('LICENSE', 'NOTICE.md', 'dependencies.lock')) {
    Add-Copy (Get-VerifiedItem $RepositoryRoot $name) $name
}
$licenseDirectory = Get-VerifiedItem $RepositoryRoot 'LICENSES' -Directory
$licenseFiles = @(Get-ChildItem -LiteralPath $licenseDirectory.FullName -Recurse -File -Force)
if ($licenseFiles.Count -eq 0) { throw 'LICENSES must not be empty.' }
foreach ($file in $licenseFiles) {
    $relative = $file.FullName.Substring($RepositoryRoot.Length + 1).Replace('\', '/')
    Add-Copy (Get-VerifiedItem $RepositoryRoot $relative) $relative
}

if (-not (Test-Path -LiteralPath $DistRoot)) {
    New-Item -ItemType Directory -Path $DistRoot | Out-Null
}
Get-VerifiedItem $RepositoryRoot 'dist' -Directory | Out-Null
$releaseName = 'release-n16r8-' + [Guid]::NewGuid().ToString('N')
$stage = Join-Path $DistRoot $releaseName
$zipPath = "$stage.zip"
$zipHashPath = "$zipPath.sha256"
New-Item -ItemType Directory -Path $stage | Out-Null

try {
    foreach ($copy in $plan) {
        $destination = Join-Path $stage $copy.Relative
        $parent = Split-Path -Parent $destination
        if (-not (Test-Path -LiteralPath $parent)) {
            New-Item -ItemType Directory -Path $parent | Out-Null
        }
        Copy-Item -LiteralPath $copy.Source -Destination $destination
        if ((Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant() -ne $copy.Hash) {
            throw "Source changed while packaging: $($copy.Relative)"
        }
    }
    $instructions = @'
ControllerBridge ESP32-S3 N16R8 raw firmware

Board: ESP32-S3, 16 MB DIO flash at 80 MHz, 8 MB Octal SPI PSRAM.
This package was assembled from an N16R8 build, not hardware-tested here.
Extract the entire ZIP before use. Do not flash only the application .bin.

With Python esptool installed, from the extracted directory:
python -m esptool --chip esp32s3 --port YOUR_PORT --before default_reset --after hard_reset write_flash "@flash_args"

Replace YOUR_PORT with the board's flashing serial port. This writes the
bootloader, partition table, and every image listed in flasher_args.json.
The packaging script does not connect to or flash hardware.
SHA256SUMS.txt covers every payload file except the checksum list itself.
Preserve LICENSE, NOTICE.md, LICENSES/, and dependencies.lock.
'@
    [IO.File]::WriteAllText((Join-Path $stage 'README.txt'), $instructions, [Text.Encoding]::ASCII)
    $sumLines = @(Get-ChildItem -LiteralPath $stage -Recurse -File | ForEach-Object {
        $relative = $_.FullName.Substring($stage.Length + 1).Replace('\', '/')
        $hash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        "$hash  $relative"
    } | Sort-Object)
    [IO.File]::WriteAllLines((Join-Path $stage 'SHA256SUMS.txt'), $sumLines, [Text.Encoding]::ASCII)
    Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::Open($zipPath, [IO.Compression.ZipArchiveMode]::Create)
    try {
        # .NET Framework's CreateFromDirectory can emit Windows backslashes.
        foreach ($file in (Get-ChildItem -LiteralPath $stage -Recurse -File | Sort-Object FullName)) {
            $relative = $file.FullName.Substring($stage.Length + 1).Replace('\', '/')
            [IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
                $archive, $file.FullName, $relative, [IO.Compression.CompressionLevel]::Optimal) | Out-Null
        }
    }
    finally { $archive.Dispose() }
    $zipHash = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash.ToLowerInvariant()
    [IO.File]::WriteAllText($zipHashPath,
        "$zipHash  $releaseName.zip`n", [Text.Encoding]::ASCII)
}
catch {
    # Only remove this invocation's freshly allocated paths under the repo dist.
    foreach ($path in @($stage, $zipPath, $zipHashPath)) {
        $fullPath = [IO.Path]::GetFullPath($path)
        if ((Split-Path -Parent $fullPath) -cne $DistRoot -or
            (Split-Path -Leaf $fullPath) -notlike "$releaseName*") {
            throw "Refusing cleanup outside the allocated release paths: $path"
        }
        if (Test-Path -LiteralPath $fullPath) {
            Remove-Item -LiteralPath $fullPath -Recurse -Force
        }
    }
    throw
}

[pscustomobject]@{
    Board = $Board
    ImageCount = $images.Count
    StageDirectory = $stage
    ZipPath = $zipPath
    Sha256 = $zipHash
    ChecksumFile = $zipHashPath
}
