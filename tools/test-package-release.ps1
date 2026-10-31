#Requires -Version 5.1
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$distRoot = Join-Path $repositoryRoot 'dist'
$packageScript = Join-Path $PSScriptRoot 'package-release.ps1'
$testRoot = Join-Path $distRoot ('test-package-' + [Guid]::NewGuid().ToString('N'))
$cleanup = New-Object 'System.Collections.Generic.List[string]'
$cleanup.Add($testRoot)
$passes = 0

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function New-Fixture([string]$Name, [string]$Board = 'n16r8', [switch]$NoCache) {
    $root = Join-Path $testRoot $Name
    $profile = @{ n4=@(4,'none'); n8=@(8,'none'); n8r2=@(8,'quad'); n8r8=@(8,'octal'); n16r8=@(16,'octal'); esp32supermini=@(4,'quad') }[$Board]
    $flashSize = "$($profile[0])MB"
    $project = "ns2pro_bridge_esp32s3_$Board"
    $application = "$project.bin"
    New-Item -ItemType Directory -Path (Join-Path $root 'bootloader') -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $root 'partition_table') -Force | Out-Null
    [IO.File]::WriteAllBytes((Join-Path $root 'bootloader/bootloader.bin'), [byte[]](1, 2, 3))
    [IO.File]::WriteAllBytes((Join-Path $root 'partition_table/partition-table.bin'), [byte[]](4, 5, 6))
    $image = [byte[]]::new(512)
    $image[0] = 0xe9; $image[1] = 1; $image[12] = 9
    [BitConverter]::GetBytes([uint32]256).CopyTo($image, 28)
    ([byte[]](0x32,0x54,0xcd,0xab)).CopyTo($image,0x20)
    [Text.Encoding]::ASCII.GetBytes($project.Substring(0,[Math]::Min(31,$project.Length))).CopyTo($image,0x50)
    [IO.File]::WriteAllBytes((Join-Path $root $application), $image)
    if (-not $NoCache) { [IO.File]::WriteAllLines((Join-Path $root 'CMakeCache.txt'), @(
        "NS2_ESP32S3_BOARD:STRING=$Board"
        'IDF_TARGET:STRING=esp32s3'
        "CMAKE_PROJECT_NAME:STATIC=$project"
    )) }
    $config = @("CONFIG_ESPTOOLPY_FLASHSIZE=`"$flashSize`"")
    if ($profile[1] -ne 'none') {
        $mode = if ($profile[1] -eq 'octal') { 'OCT' } else { 'QUAD' }
        $config += @('CONFIG_SPIRAM=y', "CONFIG_SPIRAM_MODE_$mode=y")
    }
    [IO.File]::WriteAllLines((Join-Path $root 'sdkconfig'), $config)
    $flash = [ordered]@{
        write_flash_args = @('--flash_mode', 'dio', '--flash_size', $flashSize, '--flash_freq', '80m')
        flash_settings = @{ flash_mode = 'dio'; flash_size = $flashSize; flash_freq = '80m' }
        flash_files = [ordered]@{
            '0x0' = 'bootloader/bootloader.bin'
            '0x8000' = 'partition_table/partition-table.bin'
            '0x10000' = $application
        }
        bootloader = @{ offset = '0x0'; file = 'bootloader/bootloader.bin'; encrypted = 'false' }
        'partition-table' = @{ offset = '0x8000'; file = 'partition_table/partition-table.bin'; encrypted = 'false' }
        app = @{ offset = '0x10000'; file = $application; encrypted = 'false' }
        extra_esptool_args = @{ chip = 'esp32s3'; before = 'default_reset'; after = 'hard_reset'; stub = $true }
    }
    Write-FlashJson $root $flash
    [IO.File]::WriteAllLines((Join-Path $root 'flash_args'), @(
        "--flash_mode dio --flash_freq 80m --flash_size $flashSize"
        '0x0 bootloader/bootloader.bin'
        "0x10000 $application"
        '0x8000 partition_table/partition-table.bin'
    ))
    return [pscustomobject]@{ Root = $root; Flash = $flash }
}

function Write-FlashJson([string]$Root, [object]$Flash) {
    [IO.File]::WriteAllText((Join-Path $Root 'flasher_args.json'),
        ($Flash | ConvertTo-Json -Depth 10), [Text.Encoding]::UTF8)
}

function Assert-Rejected([string]$Name, [scriptblock]$Action, [string]$ExpectedMessage) {
    $errorMessage = $null
    $result = $null
    try { $result = & $Action }
    catch { $errorMessage = $_.Exception.Message }
    if ($null -ne $result) {
        foreach ($path in @($result.StageDirectory, $result.ZipPath, $result.ChecksumFile)) {
            $cleanup.Add($path)
        }
    }
    Assert-True ($null -ne $errorMessage -and $errorMessage -match $ExpectedMessage) `
        "Expected rejection '$Name' matching '$ExpectedMessage'; received: $errorMessage"
    $script:passes++
}

try {
    if (-not (Test-Path -LiteralPath $distRoot)) { New-Item -ItemType Directory -Path $distRoot | Out-Null }
    $distItem = Get-Item -LiteralPath $distRoot -Force
    Assert-True (-not ($distItem.Attributes -band [IO.FileAttributes]::ReparsePoint)) 'dist must not be a reparse point.'
    New-Item -ItemType Directory -Path $testRoot | Out-Null

    Assert-Rejected 'explicit build directory' { & $packageScript } 'explicitly supplied'
    Assert-Rejected 'unsupported board option' {
        & $packageScript -BuildDirectory $testRoot -Board unknown
    } 'ValidateSet|validation|unknown'

    $fixture = New-Fixture 'wrong-board'
    [IO.File]::WriteAllText((Join-Path $fixture.Root 'CMakeCache.txt'), 'NS2_ESP32S3_BOARD:STRING=n8r8')
    Assert-Rejected 'wrong cached board' {
        & $packageScript -BuildDirectory $fixture.Root
    } 'proof failed'

    foreach ($fault in @('image-magic','chip','descriptor-magic','project','renamed-board','unterminated-project','short-image')) {
        $fixture = New-Fixture "compiled-$fault" -NoCache
        $appPath = Join-Path $fixture.Root 'ns2pro_bridge_esp32s3_n16r8.bin'
        $image = [IO.File]::ReadAllBytes($appPath)
        switch ($fault) {
            'image-magic' { $image[0] = 0 }
            'chip' { $image[12] = 0 }
            'descriptor-magic' { $image[0x20] = 0 }
            'project' { $image[0x50] = [byte][char]'X' }
            'renamed-board' {
                [Array]::Clear($image,0x50,32)
                [Text.Encoding]::ASCII.GetBytes('ns2pro_bridge_esp32s3_n8r8').CopyTo($image,0x50)
            }
            'unterminated-project' { $image[0x6f] = [byte][char]'X' }
            'short-image' { $image = [byte[]](0xe9,1,0) }
        }
        [IO.File]::WriteAllBytes($appPath,$image)
        Assert-Rejected "compiled $fault without cache" {
            & $packageScript -BuildDirectory $fixture.Root
        } 'Compiled application|Compiled project identity'
    }
    $fixture = New-Fixture 'compiled-mismatch-with-cache'
    $appPath = Join-Path $fixture.Root 'ns2pro_bridge_esp32s3_n16r8.bin'
    $image = [IO.File]::ReadAllBytes($appPath); $image[0x50] = 0
    [IO.File]::WriteAllBytes($appPath,$image)
    Assert-Rejected 'cache cannot replace compiled identity' {
        & $packageScript -BuildDirectory $fixture.Root
    } 'Compiled project identity'

    foreach ($fault in @('missing','flash','psram')) {
        $fixture = New-Fixture "config-$fault" -NoCache
        $configPath = Join-Path $fixture.Root 'sdkconfig'
        switch ($fault) {
            'missing' { Remove-Item -LiteralPath $configPath }
            'flash' { [IO.File]::WriteAllText($configPath,'CONFIG_ESPTOOLPY_FLASHSIZE="8MB"') }
            'psram' { [IO.File]::WriteAllText($configPath,'CONFIG_ESPTOOLPY_FLASHSIZE="16MB"') }
        }
        Assert-Rejected "sdkconfig $fault without cache" {
            & $packageScript -BuildDirectory $fixture.Root
        } 'sdkconfig'
    }

    foreach ($board in @('n4','n8','n8r2','n8r8','n16r8','esp32supermini')) {
        $fixture = New-Fixture "cacheless-$board" -Board $board -NoCache
        $result = & $packageScript -BuildDirectory $fixture.Root -Board $board
        foreach ($path in @($result.StageDirectory,$result.ZipPath,$result.ChecksumFile)) { $cleanup.Add($path) }
        Assert-True ($result.Board -ceq $board) 'Cache-less compiled board identity was not retained.'
        Assert-True (-not (Test-Path -LiteralPath (Join-Path $fixture.Root 'CMakeCache.txt'))) 'Packager fabricated a CMake cache.'
        $passes++
    }

    foreach ($field in @('chip', 'flash_size')) {
        $fixture = New-Fixture "wrong-$field"
        if ($field -eq 'chip') { $fixture.Flash.extra_esptool_args.chip = 'esp32' }
        else { $fixture.Flash.flash_settings.flash_size = '8MB' }
        Write-FlashJson $fixture.Root $fixture.Flash
        Assert-Rejected "wrong $field" {
            & $packageScript -BuildDirectory $fixture.Root
        } $field
    }

    $index = 0
    foreach ($unsafe in @('../outside.bin', 'C:\outside.bin', '\\server\share\outside.bin',
        '/outside.bin', 'bootloader/../outside.bin', 'bootloader.bin:stream', 'bootloader//x.bin')) {
        $fixture = New-Fixture "unsafe-$index"
        $fixture.Flash.flash_files['0x0'] = $unsafe
        Write-FlashJson $fixture.Root $fixture.Flash
        Assert-Rejected "unsafe path $index" {
            & $packageScript -BuildDirectory $fixture.Root
        } 'Unsafe relative path'
        $index++
    }

    $fixture = New-Fixture 'missing-image'
    Remove-Item -LiteralPath (Join-Path $fixture.Root 'bootloader/bootloader.bin')
    Assert-Rejected 'missing bootloader' {
        & $packageScript -BuildDirectory $fixture.Root
    } 'bootloader'

    $fixture = New-Fixture 'bad-flash-args'
    [IO.File]::WriteAllText((Join-Path $fixture.Root 'flash_args'),
        '--flash_mode dio --flash_freq 80m --flash_size 16MB 0x0 ../outside.bin 0x10000 ns2pro_bridge_esp32s3_n16r8.bin 0x8000 partition_table/partition-table.bin')
    Assert-Rejected 'unsafe flash_args reference' {
        & $packageScript -BuildDirectory $fixture.Root
    } 'Unsafe relative path'

    $fixture = New-Fixture 'valid'
    $result = & $packageScript -BuildDirectory $fixture.Root -Board n16r8
    foreach ($path in @($result.StageDirectory, $result.ZipPath, $result.ChecksumFile)) { $cleanup.Add($path) }
    Assert-True ($result.ImageCount -eq 3) 'Expected three packaged images.'
    Assert-True ((Get-FileHash -LiteralPath $result.ZipPath -Algorithm SHA256).Hash.ToLowerInvariant() -ceq $result.Sha256) 'ZIP SHA256 mismatch.'
    $zip = [IO.Compression.ZipFile]::OpenRead($result.ZipPath)
    try {
        $names = @($zip.Entries | ForEach-Object { $_.FullName })
        Assert-True (@($names | Where-Object { $_.Contains('\') }).Count -eq 0) 'ZIP entry paths must use forward slashes.'
        foreach ($name in @('bootloader/bootloader.bin', 'partition_table/partition-table.bin',
            'ns2pro_bridge_esp32s3_n16r8.bin', 'flasher_args.json', 'flash_args', 'LICENSE',
            'NOTICE.md', 'dependencies.lock', 'README.txt', 'SHA256SUMS.txt')) {
            Assert-True ($name -cin $names) "ZIP is missing $name"
        }
        Assert-True (@($names | Where-Object { $_ -like 'LICENSES/*' }).Count -gt 0) 'ZIP is missing license copies.'
        $checksumCount = 0
        foreach ($line in Get-Content -LiteralPath (Join-Path $result.StageDirectory 'SHA256SUMS.txt')) {
            Assert-True ($line -match '^([0-9a-f]{64})  (.+)$') 'Malformed SHA256 manifest entry.'
            $expected = $Matches[1]
            $name = $Matches[2]
            $entry = $zip.GetEntry($name)
            Assert-True ($null -ne $entry) "Manifest file absent from ZIP: $name"
            $stream = $entry.Open()
            $sha = [Security.Cryptography.SHA256]::Create()
            try { $actual = [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-', '').ToLowerInvariant() }
            finally { $sha.Dispose(); $stream.Dispose() }
            Assert-True ($actual -ceq $expected) "Archived payload checksum mismatch: $name"
            $checksumCount++
        }
        Assert-True ($checksumCount -eq ($names.Count - 1)) 'SHA256 manifest does not cover the full ZIP payload.'
    }
    finally { $zip.Dispose() }
    $passes++
    Write-Output "PASS: $passes offline packaging checks; no hardware accessed."
}
finally {
    foreach ($path in $cleanup) {
        $fullPath = [IO.Path]::GetFullPath($path)
        $leaf = Split-Path -Leaf $fullPath
        if ((Split-Path -Parent $fullPath) -cne $distRoot -or
            ($fullPath -cne $testRoot -and $leaf -notmatch '^release-(n4|n8|n8r2|n8r8|n16r8|esp32supermini)-[0-9a-f]{32}(\.zip(\.sha256)?)?$')) {
            throw "Refusing cleanup outside test-owned dist paths: $path"
        }
        if (Test-Path -LiteralPath $fullPath) { Remove-Item -LiteralPath $fullPath -Recurse -Force }
    }
}
