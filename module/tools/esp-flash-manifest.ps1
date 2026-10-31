#Requires -Version 7.2
# Only fixed ESP32-S3 image plans are accepted, never executable arguments.
function Test-EspFlashManifest([string]$ManifestPath) {
    $path = Get-SafeFullPath $ManifestPath
    $root = Split-Path -Parent $path
    $m = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
    $boards = @{
        n4=@(4,0,'none'); n8=@(8,0,'none'); n8r2=@(8,2,'quad')
        n8r8=@(8,8,'octal'); n16r8=@(16,8,'octal'); esp32supermini=@(4,2,'quad')
    }
    if($m.schemaVersion -ne 1 -or $m.chip -cne 'esp32s3' -or -not $boards.ContainsKey([string]$m.board)) { throw 'Invalid ESP32-S3 manifest identity.' }
    $memory=$boards[[string]$m.board]
    if($m.flashSizeMb -ne $memory[0] -or $m.psramSizeMb -ne $memory[1] -or $m.psramMode -cne $memory[2] -or
       $m.flashMode -cne 'dio' -or $m.flashFrequencyMhz -ne 80 -or $m.baudRate -ne 460800) { throw 'ESP32-S3 board memory/flash settings mismatch.' }
    if(@($m.files).Count -ne 3) { throw 'ESP32-S3 requires exactly bootloader, partition, application.' }
    $required=@{bootloader=0;partition=0x8000;application=0x10000}
    $seen=@{}
    $images=@{}
    foreach($f in $m.files) {
        if(-not $required.ContainsKey([string]$f.kind) -or $seen.ContainsKey([string]$f.kind) -or $f.address -notmatch '^0x[0-9a-fA-F]+$') { throw 'Invalid/duplicate image kind or offset.' }
        $address=[Convert]::ToUInt32($f.address.Substring(2),16)
        if($address -ne $required[[string]$f.kind]) { throw 'Unexpected ESP image offset.' }
        $file=Join-SafePath $root ([string]$f.path)
        $bytes=[IO.File]::ReadAllBytes($file)
        if($bytes.Length -ne $f.size -or $f.size -le 0 -or $address+[long]$f.size -gt $m.flashSizeMb*1MB -or
           $f.sha256 -cnotmatch '^[a-f0-9]{64}$' -or (Get-FileHash -LiteralPath $file).Hash.ToLowerInvariant() -cne $f.sha256) { throw 'ESP image size/hash/capacity mismatch.' }
        if($f.kind -ne 'partition') {
            if($bytes.Length -lt 24 -or $bytes[0] -ne 0xe9 -or $bytes[1] -lt 1 -or $bytes[1] -gt 16 -or
               [BitConverter]::ToUInt16($bytes,12) -ne 9) { throw 'Image is not an ESP32-S3 firmware image.' }
        }
        $seen[[string]$f.kind]=$true
        $images[[string]$f.kind]=[pscustomobject]@{Address=$address;Bytes=$bytes;Size=$bytes.Length}
    }
    if($images.bootloader.Size -gt 0x8000 -or $images.partition.Size -gt 0x1000) { throw 'ESP bootloader/partition images overlap reserved regions.' }
    $partition=$images.partition.Bytes
    $appFound=$false
    for($offset=0;$offset+32 -le $partition.Length;$offset+=32) {
        $magic=[BitConverter]::ToUInt16($partition,$offset)
        if($magic -eq 0xffff -or $magic -eq 0xebeb) { break }
        if($magic -ne 0x50aa) { throw 'Invalid ESP partition table magic.' }
        $start=[BitConverter]::ToUInt32($partition,$offset+4)
        $size=[BitConverter]::ToUInt32($partition,$offset+8)
        if($start+[long]$size -gt $m.flashSizeMb*1MB) { throw 'Partition exceeds flash capacity.' }
        if($partition[$offset+2] -eq 0 -and $start -eq 0x10000) {
            if($images.application.Size -gt $size) { throw 'Application exceeds its partition.' }
            $appFound=$true
        }
    }
    if(-not $appFound) { throw 'Application partition missing at 0x10000.' }
    return $m
}
