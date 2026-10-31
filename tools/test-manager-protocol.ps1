#Requires -Version 7.2
[CmdletBinding()]
param([string]$Compiler = 'g++')
$ErrorActionPreference = 'Stop'
$Compiler = (Get-Command $Compiler -ErrorAction Stop).Source
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$work = Join-Path $root 'test-results/manager-protocol'
New-Item -ItemType Directory -Force -Path "$work/stubs/freertos" | Out-Null
$stubs = @{
    'esp_err.h' = '#pragma once
using esp_err_t = int;
constexpr int ESP_OK = 0;'
    'esp_app_desc.h' = '#pragma once
struct esp_app_desc_t { const char *version; };
inline const esp_app_desc_t *esp_app_get_description() { static esp_app_desc_t a{"test"}; return &a; }'
    'esp_mac.h' = '#pragma once
#include "esp_err.h"
#include <cstdint>
#include <cstring>
inline int esp_efuse_mac_get_default(uint8_t *p) { std::memset(p, 0x12, 6); return 0; }'
    'esp_log.h' = '#pragma once
#define ESP_LOGI(...)
#define ESP_LOGE(...)
#define ESP_LOGD(...)'
    'esp_timer.h' = '#pragma once
#include <cstdint>
extern int64_t mock_time;
inline int64_t esp_timer_get_time() { return mock_time; }'
    'freertos/FreeRTOS.h' = '#pragma once
using portMUX_TYPE=int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))'
}
foreach($name in $stubs.Keys) { [IO.File]::WriteAllText((Join-Path "$work/stubs" $name),$stubs[$name]) }
& $Compiler -std=c++17 -Wall -Wextra -Werror -I "$work/stubs" -I "$root/main" "$root/main/manager_protocol.cpp" "$root/tools/test_manager_protocol.cpp" -o "$work/test.exe"
if($LASTEXITCODE -ne 0) { throw 'Protocol host compilation failed.' }
$originalPath = $env:Path
try {
    $env:Path = (Split-Path $Compiler) + [IO.Path]::PathSeparator + $originalPath
    & "$work/test.exe"
    if($LASTEXITCODE -ne 0) { throw 'Protocol tests failed.' }
} finally { $env:Path = $originalPath }
