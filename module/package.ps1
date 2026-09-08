#Requires -Version 7.2
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'tools/package-module.ps1') `
    -ExpectedId 'esp32s3-ns2-bridge' -Method 'None'
