#Requires -Version 7.2
[CmdletBinding()]
param([Parameter(Mandatory)][string[]]$BuildDirectories)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'tools/package-paths.ps1')
. (Join-Path $PSScriptRoot 'tools/esp-flash-manifest.ps1')
$repo=Get-SafeFullPath (Join-Path $PSScriptRoot '..')
$manifest=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'module.json') -Raw | ConvertFrom-Json
if($manifest.id -cne 'esp32s3-ns2-bridge') { throw 'Unexpected module identity.' }
$stageRoot=Join-SafePath $repo 'dist/stage'
$stage=Join-SafePath $stageRoot ('esp-module-'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $stage -Force | Out-Null
$firmware=[Collections.Generic.List[object]]::new()
$selected=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
$memory=@{n4=@(4,0,'none');n8=@(8,0,'none');n8r2=@(8,2,'quad');n8r8=@(8,8,'octal');n16r8=@(16,8,'octal');esp32supermini=@(4,2,'quad')}
try {
    foreach($directory in $BuildDirectories) {
        $build=Get-SafeFullPath $directory
        $flash=Get-Content -LiteralPath (Join-SafePath $build 'flasher_args.json') -Raw | ConvertFrom-Json
        # The filename selects the requested profile; the raw packager verifies
        # the corresponding compiled project identity, even without a CMake cache.
        if($flash.app.file -isnot [string] -or
           $flash.app.file -cnotmatch '^ns2pro_bridge_esp32s3_(n4|n8|n8r2|n8r8|n16r8|esp32supermini)\.bin$') {
            throw 'Missing strict board application filename in flasher_args.json.'
        }
        $board=$Matches[1]
        if(-not $memory.ContainsKey($board) -or -not $selected.Add($board)) { throw 'Unknown or duplicate board build.' }
        $raw=& (Join-Path $repo 'tools/package-release.ps1') -BuildDirectory $build -Board $board
        $config=Get-Content -LiteralPath (Join-SafePath $build 'sdkconfig') -Raw
        $mem=$memory[$board]
        if($config -notmatch ('(?m)^CONFIG_ESPTOOLPY_FLASHSIZE="'+$mem[0]+'MB"\r?$')) { throw 'sdkconfig flash size disagrees with board.' }
        if($mem[1] -gt 0) {
            $mode=if($mem[2] -eq 'octal'){'OCT'}else{'QUAD'}
            if($config -notmatch '(?m)^CONFIG_SPIRAM=y\r?$' -or $config -notmatch ('(?m)^CONFIG_SPIRAM_MODE_'+$mode+'=y\r?$')) { throw 'sdkconfig PSRAM mode disagrees with board.' }
        } elseif($config -match '(?m)^CONFIG_SPIRAM=y\r?$') { throw 'Unexpected PSRAM requirement.' }
        if (@($flash.flash_files.PSObject.Properties).Count -ne 3) {
            throw 'Manager module requires the validated three-image layout; extra IDF images must not be silently omitted.'
        }
        $files=[Collections.Generic.List[object]]::new()
        foreach($pair in @(@('bootloader','bootloader'),@('partition-table','partition'),@('app','application'))) {
            $entry=$flash.($pair[0]); $relative=Get-SafeRelativePath ([string]$entry.file)
            $image=Join-SafePath $build $relative
            Copy-StageFile $image $stage "firmware/$board/$relative"
            $files.Add([ordered]@{kind=$pair[1];path=$relative;address=$entry.offset;sha256=(Get-FileHash -LiteralPath $image).Hash.ToLowerInvariant();size=(Get-Item -LiteralPath $image).Length})
        }
        $plan=[ordered]@{schemaVersion=1;chip='esp32s3';board=$board;flashSizeMb=$mem[0];psramSizeMb=$mem[1];psramMode=$mem[2];flashMode='dio';flashFrequencyMhz=80;baudRate=460800;files=$files.ToArray()}
        $planPath=Join-SafePath $stage "firmware/$board/flash-manifest.json"
        [IO.File]::WriteAllText($planPath,($plan|ConvertTo-Json -Depth 12),[Text.UTF8Encoding]::new($false))
        $null=Test-EspFlashManifest $planPath
        $entry=@($manifest.firmware | Where-Object { $_.boardIds.Count -eq 1 -and $_.boardIds[0] -ceq "esp32s3-$board" })
        if($entry.Count -ne 1 -or $entry[0].flashMethod -cne 'EspressifSerial' -or $entry[0].artifactRelativePath -cne "firmware/$board/flash-manifest.json") { throw 'Board firmware declaration mismatch.' }
        $firmware.Add($entry[0])
        Write-Host "Validated $board image set; raw archive: $($raw.ZipPath)"
    }
    $manifest.firmware=$firmware.ToArray()
    [IO.File]::WriteAllText((Join-Path $stage 'module.json'),($manifest|ConvertTo-Json -Depth 100),[Text.UTF8Encoding]::new($false))
    foreach($name in @('LICENSE','NOTICE.md','dependencies.lock')) { Copy-StageFile (Join-SafePath $repo $name) $stage $name }
    foreach($file in @(Get-SafeTreeFiles (Join-SafePath $repo 'LICENSES'))) {
        Copy-StageFile $file.FullName $stage ([IO.Path]::GetRelativePath($repo,$file.FullName).Replace('\','/'))
    }
    & (Join-Path $PSScriptRoot 'tools/pack-module-directory.ps1') -SourceDirectory $stage
} finally { Remove-PackageStage $stageRoot $stage }
