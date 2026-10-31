#Requires -Version 7.2
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$moduleRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
. (Join-Path $moduleRoot 'tools/package-paths.ps1')
$fixture = Join-SafePath $PSScriptRoot ('.work-' + [Guid]::NewGuid().ToString('N'))
$repo = Join-SafePath $fixture 'repo'
$fixtureModule = Join-SafePath $repo 'module'
$checks = 0

function Require([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
    $script:checks++
}
function Reject([scriptblock]$Action, [string]$Pattern) {
    $caught = $null
    try { & $Action | Out-Null }
    catch { $caught = $_.Exception.Message }
    Require ($null -ne $caught -and $caught -match $Pattern) "Expected '$Pattern'; got '$caught'."
}
function Write-Fixture([string]$Relative, [string]$Text) {
    $path = Join-SafePath $fixture $Relative
    New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($path)) -Force | Out-Null
    [IO.File]::WriteAllText($path, $Text, [Text.UTF8Encoding]::new($false))
}

New-Item -ItemType Directory -Path $fixtureModule -Force | Out-Null
try {
    foreach ($name in @('module.json', 'package.ps1')) {
        Copy-StageFile (Join-SafePath $moduleRoot $name) $fixtureModule $name
    }
    foreach ($directory in @('tools', 'schemas')) {
        $source = Join-SafePath $moduleRoot $directory
        foreach ($file in @(Get-SafeTreeFiles $source)) {
            Copy-StageFile $file.FullName $fixtureModule (
                "$directory/" + [IO.Path]::GetRelativePath($source, $file.FullName).Replace('\', '/'))
        }
    }
    Copy-StageFile (Join-Path $moduleRoot '../tools/package-release.ps1') $repo 'tools/package-release.ps1'
    Write-Fixture 'repo/dependencies.lock' 'fixture dependency lock'
    Write-Fixture 'repo/LICENSE' 'TEST FIXTURE ONLY. Not a redistributable firmware package.'
    Write-Fixture 'repo/NOTICE.md' 'Synthetic packaging test; no device or build is involved.'
    Write-Fixture 'repo/LICENSES/fixture.txt' 'Fixture license payload.'
    Write-Fixture 'repo/LICENSES/.hidden-fixture.txt' 'Hidden files must be included and hashed.'
    Write-Fixture 'outside/sentinel.txt' 'UNCHANGED'
    $sentinel = Join-SafePath $fixture 'outside/sentinel.txt'
    $originalSentinel = (Get-FileHash -LiteralPath $sentinel).Hash
    if ($IsWindows) {
        [IO.File]::SetAttributes((Join-SafePath $repo 'LICENSES/.hidden-fixture.txt'),
            [IO.FileAttributes]::Hidden)
    }

    $manifestPath = Join-SafePath $fixtureModule 'module.json'
    $originalManifest = [IO.File]::ReadAllBytes($manifestPath)
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    $id = [string]$manifest.id
    $method = [string]$manifest.firmware[0].flashMethod
    $packageScript = Join-SafePath $fixtureModule 'package.ps1'
    $arguments = @{}
    if ($method -eq 'EspressifSerial') {
        $inputRoot=Join-SafePath $fixture 'inputs'
        New-Item -ItemType Directory -Path $inputRoot -Force | Out-Null
        $image=[byte[]]::new(512); $image[0]=0xe9; $image[1]=1; $image[12]=9
        [BitConverter]::GetBytes([uint32]256).CopyTo($image,28)
        ([byte[]](0x32,0x54,0xcd,0xab)).CopyTo($image,0x20)
        [Text.Encoding]::ASCII.GetBytes('ns2pro_bridge_esp32s3_n16r8').CopyTo($image,0x50)
        [IO.File]::WriteAllBytes((Join-Path $inputRoot 'boot.bin'),$image)
        [IO.File]::WriteAllBytes((Join-Path $inputRoot 'ns2pro_bridge_esp32s3_n16r8.bin'),$image)
        $part=[byte[]]::new(64); $part[0]=0xaa; $part[1]=0x50
        [BitConverter]::GetBytes([uint32]0x10000).CopyTo($part,4)
        [BitConverter]::GetBytes([uint32]0x100000).CopyTo($part,8)
        $part[32]=0xff; $part[33]=0xff
        [IO.File]::WriteAllBytes((Join-Path $inputRoot 'part.bin'),$part)
        # Model CI artifacts: no CMakeCache, but mandatory compiled app identity.
        Write-Fixture 'inputs/sdkconfig' "CONFIG_ESPTOOLPY_FLASHSIZE=`"16MB`"`nCONFIG_SPIRAM=y`nCONFIG_SPIRAM_MODE_OCT=y"
        $flash=@{
            write_flash_args=@('--flash_mode','dio','--flash_size','16MB','--flash_freq','80m')
            flash_settings=@{flash_mode='dio';flash_size='16MB';flash_freq='80m'}
            flash_files=@{'0x0'='boot.bin';'0x8000'='part.bin';'0x10000'='ns2pro_bridge_esp32s3_n16r8.bin'}
            bootloader=@{offset='0x0';file='boot.bin';encrypted='false'}
            'partition-table'=@{offset='0x8000';file='part.bin';encrypted='false'}
            app=@{offset='0x10000';file='ns2pro_bridge_esp32s3_n16r8.bin';encrypted='false'}
            extra_esptool_args=@{chip='esp32s3';before='default_reset';after='hard_reset';stub=$true}
        }
        Write-Fixture 'inputs/flasher_args.json' ($flash|ConvertTo-Json -Depth 8)
        Write-Fixture 'inputs/flash_args' "--flash_mode dio --flash_freq 80m --flash_size 16MB`n0x0 boot.bin`n0x8000 part.bin`n0x10000 ns2pro_bridge_esp32s3_n16r8.bin"
        $arguments.BuildDirectories=@($inputRoot)
    } elseif ($method -eq 'SifliSerial') {
        Write-Fixture 'inputs/boot.bin' 'SYNTHETIC BOOT FILE'
        Write-Fixture 'inputs/segments/main.bin' 'SYNTHETIC MAIN FILE'
        $parameters = @{
            chip = 'SF32LB52'; memory = 'nor'
            write_flash = @{ files = @(
                @{ address = '0x10000000'; path = 'boot.bin' },
                @{ address = '0x12000000'; path = 'segments/main.bin' }
            ) }
        }
        Write-Fixture 'inputs/sftool_param.json' ($parameters | ConvertTo-Json -Depth 10)
        $arguments.BuildDirectory = Join-SafePath $fixture 'inputs'
    } elseif ($method -eq 'PicoUf2') {
        $uf2 = [byte[]]::new(512)
        foreach ($pair in @(
            @(0, [uint32]0x0A324655), @(4, [Convert]::ToUInt32('9E5D5157', 16)),
            @(16, [uint32]256), @(24, [uint32]1), @(508, [uint32]0x0AB16F30)
        )) {
            [BitConverter]::GetBytes([uint32]$pair[1]).CopyTo($uf2, [int]$pair[0])
        }
        $arguments.Uf2Path = Join-SafePath $fixture 'synthetic.uf2'
        [IO.File]::WriteAllBytes($arguments.Uf2Path, $uf2)
    }

    # Run the copied contract from a foreign working directory, with no manager tree.
    Push-Location (Join-SafePath $fixture 'outside')
    try { & $packageScript @arguments }
    finally { Pop-Location }
    $package = Join-SafePath $repo "dist/modules/$id-$($manifest.moduleVersion).cbmodule"
    Require (Test-Path -LiteralPath $package -PathType Leaf) 'Expected package was not emitted.'
    Test-ModuleArchive $package
    $checks++
    $originalPackage = (Get-FileHash -LiteralPath $package).Hash
    Reject { & $packageScript @arguments } 'already exists'
    Require ((Get-FileHash -LiteralPath $package).Hash -eq $originalPackage) 'Existing package changed.'

    $payload = Join-SafePath $fixture 'payload'
    [IO.Compression.ZipFile]::ExtractToDirectory($package, $payload)
    Require (Test-Path -LiteralPath (Join-SafePath $payload 'LICENSES/.hidden-fixture.txt')) `
        'Hidden license was omitted.'
    $packedManifest=Get-Content -LiteralPath (Join-SafePath $payload 'module.json') -Raw | ConvertFrom-Json
    Require ($packedManifest.id -ceq $manifest.id -and $packedManifest.moduleVersion -ceq $manifest.moduleVersion) 'Manifest identity changed.'
    Require (@($packedManifest.firmware).Count -eq 1) 'Package must declare only the explicitly built board.'
    foreach ($name in @('LICENSE', 'NOTICE.md', 'LICENSES/fixture.txt')) {
        Require ((Get-FileHash -LiteralPath (Join-SafePath $payload $name)).Hash -eq
            (Get-FileHash -LiteralPath (Join-SafePath $repo $name)).Hash) "Root notice not preserved: $name"
    }
    if ($method -eq 'EspressifSerial') {
        Require (-not (Test-Path -LiteralPath (Join-Path $inputRoot 'CMakeCache.txt'))) 'Packager fabricated a CMake cache.'
        . (Join-Path $moduleRoot 'tools/esp-flash-manifest.ps1')
        $planPath=Join-SafePath $payload 'firmware/n16r8/flash-manifest.json'
        $plan=Test-EspFlashManifest $planPath
        $checks++
        Require ($plan.files.Count -eq 3) 'Complete image set absent.'
        $originalPlan=[IO.File]::ReadAllText($planPath)
        foreach($invalid in @('chip','flashSizeMb','psramMode','address','path','sha256','size')) {
            $plan=$originalPlan|ConvertFrom-Json
            switch($invalid) {
                chip {$plan.chip='esp32'}
                flashSizeMb {$plan.flashSizeMb=4}
                psramMode {$plan.psramMode='quad'}
                address {$plan.files[2].address='0x8000'}
                path {$plan.files[2].path='../outside.bin'}
                sha256 {$plan.files[2].sha256=('0'*64)}
                size {$plan.files[2].size=1}
            }
            [IO.File]::WriteAllText($planPath,($plan|ConvertTo-Json -Depth 8))
            Reject { Test-EspFlashManifest $planPath } 'Invalid|mismatch|Unexpected|Unsafe'
        }
        [IO.File]::WriteAllText($planPath,$originalPlan)
        $applicationPath=Join-Path $inputRoot 'ns2pro_bridge_esp32s3_n16r8.bin'
        foreach($offset in @(0,12,0x20,0x50)) {
            $originalByte=$image[$offset]; $image[$offset]=0
            [IO.File]::WriteAllBytes($applicationPath,$image)
            Reject { & $packageScript @arguments } 'Compiled application|Compiled project identity'
            $image[$offset]=$originalByte
        }
        [IO.File]::WriteAllBytes($applicationPath,$image)
        Write-Fixture 'inputs/CMakeCache.txt' "NS2_ESP32S3_BOARD:STRING=n8r8`nIDF_TARGET:STRING=esp32s3`nCMAKE_PROJECT_NAME:STATIC=ns2pro_bridge_esp32s3_n8r8"
        Reject { & $packageScript @arguments } 'proof failed'
        Remove-Item -LiteralPath (Join-Path $inputRoot 'CMakeCache.txt')

        $originalFlash=[IO.File]::ReadAllText((Join-Path $inputRoot 'flasher_args.json'))
        $originalArgs=[IO.File]::ReadAllText((Join-Path $inputRoot 'flash_args'))
        $originalConfig=[IO.File]::ReadAllText((Join-Path $inputRoot 'sdkconfig'))
        $renamed=$originalFlash.Replace('ns2pro_bridge_esp32s3_n16r8.bin','ns2pro_bridge_esp32s3_n8r8.bin').Replace('16MB','8MB')
        Write-Fixture 'inputs/flasher_args.json' $renamed
        Write-Fixture 'inputs/flash_args' ($originalArgs.Replace('ns2pro_bridge_esp32s3_n16r8.bin','ns2pro_bridge_esp32s3_n8r8.bin').Replace('16MB','8MB'))
        Write-Fixture 'inputs/sdkconfig' ($originalConfig.Replace('16MB','8MB'))
        [IO.File]::WriteAllBytes((Join-Path $inputRoot 'ns2pro_bridge_esp32s3_n8r8.bin'),$image)
        Reject { & $packageScript @arguments } 'Compiled project identity'
        Write-Fixture 'inputs/flasher_args.json' ($originalFlash.Replace('ns2pro_bridge_esp32s3_n16r8.bin','renamed.bin'))
        Reject { & $packageScript @arguments } 'strict board application filename'
        Write-Fixture 'inputs/flasher_args.json' $originalFlash
        Write-Fixture 'inputs/flash_args' $originalArgs
        Write-Fixture 'inputs/sdkconfig' $originalConfig
    } elseif ($method -eq 'None') {
        Require (-not (Test-Path -LiteralPath (Join-SafePath $payload 'artifacts'))) `
            'Metadata-only ESP package includes firmware.'
    } elseif ($method -eq 'SifliSerial') {
        foreach ($relative in @('sftool_param.json', 'boot.bin', 'segments/main.bin')) {
            Require ((Get-FileHash -LiteralPath (Join-SafePath $payload "artifacts/$relative")).Hash -eq
                (Get-FileHash -LiteralPath (Join-SafePath $arguments.BuildDirectory $relative)).Hash) `
                "SF32 artifact changed: $relative"
        }
        foreach ($unsafe in @('../outside/sentinel.txt', 'C:\escape.bin', 'main.bin:stream')) {
            $parameters.write_flash.files[0].path = $unsafe
            Write-Fixture 'inputs/sftool_param.json' ($parameters | ConvertTo-Json -Depth 10)
            Reject { & $packageScript @arguments } 'Unsafe relative path'
        }
        $parameters.write_flash.files[0].path = 'missing.bin'
        Write-Fixture 'inputs/sftool_param.json' ($parameters | ConvertTo-Json -Depth 10)
        Reject { & $packageScript @arguments } 'Missing SF32 flash file'
    } else {
        Require ((Get-FileHash -LiteralPath (Join-SafePath $payload `
            $manifest.firmware[0].artifactRelativePath)).Hash -eq
            (Get-FileHash -LiteralPath $arguments.Uf2Path).Hash) 'UF2 bytes changed.'
        [IO.File]::WriteAllBytes($arguments.Uf2Path, [byte[]]::new(512))
        Reject { & $packageScript @arguments } 'Invalid UF2 block header'
    }

    $packer = Join-SafePath $fixtureModule 'tools/pack-module-directory.ps1'
    Reject { & $packer -SourceDirectory $payload -OutputPath $sentinel } 'OutputPath must'
    foreach ($unsafe in @('../escape.bin', '..\escape.bin', '/absolute.bin', 'C:\escape.bin',
        'C:relative.bin', '\\host\share\escape.bin', 'main.bin:stream', 'a/../b', 'a//b',
        'CON.bin', 'trailing.', "line`nbreak")) {
        Reject { Join-SafePath $fixture $unsafe } 'Unsafe relative path'
    }
    Reject { Assert-ContainedPath $repo (Join-Path $fixture 'repo-other/file') } 'outside'
    foreach($entry in $manifest.firmware) { $entry.artifactRelativePath = '../outside/sentinel.txt' }
    [IO.File]::WriteAllText($manifestPath, ($manifest | ConvertTo-Json -Depth 100))
    Reject { & $packageScript @arguments } 'Unsafe relative path|metadata-only|declaration mismatch'
    [IO.File]::WriteAllBytes($manifestPath, $originalManifest)

    if ($IsWindows) {
        $link = Join-SafePath $fixture 'junction'
        New-Item -ItemType Junction -Path $link -Target (Join-SafePath $fixture 'outside') | Out-Null
        try { Reject { Get-SafeFullPath (Join-Path $link 'sentinel.txt') } 'reparse points' }
        finally {
            # Delete the explicitly created link itself, never its target or a recursive tree.
            Remove-Item -LiteralPath $link -Force
        }
    }
    $tampered = Join-SafePath $fixture 'tampered.cbmodule'
    [IO.File]::Copy($package, $tampered, $false)
    $archive = [IO.Compression.ZipFile]::Open($tampered, [IO.Compression.ZipArchiveMode]::Update)
    try {
        $stream = $archive.GetEntry('LICENSE').Open()
        try { $stream.SetLength(0); $stream.WriteByte(88) }
        finally { $stream.Dispose() }
    }
    finally { $archive.Dispose() }
    Reject { Test-ModuleArchive $tampered } 'hash mismatch'
    Require ((Get-FileHash -LiteralPath $sentinel).Hash -eq $originalSentinel) 'Outside sentinel changed.'
    Require (@(Get-ChildItem -LiteralPath (Join-SafePath $repo 'dist/stage') -Force).Count -eq 0) `
        'Staging directories were not cleaned.'
    Write-Host "PASS $id : $checks packaging, containment and hash checks (synthetic inputs only)."
}
finally { Remove-PackageStage $PSScriptRoot $fixture }
