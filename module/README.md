# ESP32-S3 firmware module

PowerShell 7.2+ is required. The packager is independent of a Manager checkout
and never builds, probes serial ports, flashes, or downloads firmware.

```powershell
./module/package.ps1 -BuildDirectories ./build-n16r8
./module/package.ps1 -BuildDirectories ./build-n4,./build-n16r8
```

Inputs must be completed ESP-IDF 5.3.3 builds of the exact selected memory
profile. The app filename in `flasher_args.json` must exactly name one of the
six supported boards. The packager then verifies the identity compiled into
that application: ESP image magic, ESP32-S3 chip ID 9, app descriptor magic at
`0x20`, and `project_name` at `0x50` matching `ns2pro_bridge_esp32s3_<board>`.
The comparison includes the SDK's 31-character truncation and NUL padding, so
the ESP32SUPERMINI profile is checked correctly. Renaming another board's
binary does not satisfy this check.

CI artifacts may omit `CMakeCache.txt`; no replacement cache is generated.
When present, its board, target and project name must also agree. The compiled
identity check is mandatory in both cases. `sdkconfig`, `flasher_args.json`
and `flash_args` remain required and are cross-checked. Each complete set
contains ESP32-S3 bootloader, partition table and application images. Fixed
offsets, partition bounds, flash/PSRAM configuration, lengths and SHA-256
hashes are validated.

The Runtime API 2 module exposes the four USB identities, NS2Pro BLE input,
full canonical mapping and direct NSM1 management. Only board profiles actually
supplied to the packager remain in its firmware list. Firmware uses
`flashMethod: EspressifSerial`; the board's `flash-manifest.json` lives under
`firmware/<board>/` alongside its images. Manager supplies a pinned standalone
esptool backend and checks the target chip and physical flash capacity before
writing. PSRAM wiring remains an explicit board selection, not a ROM probe.

Output: `dist/modules/esp32s3-ns2-bridge-<version>.cbmodule`. Existing versions
are never overwritten. The ZIP contains one root module manifest, licensing,
pinned dependencies, firmware sets, and `MODULE-SHA256.txt` covering every
payload file. Archive entries are read back and verified before delivery.

Embedded paths must remain inside the module, with no traversal, absolute
paths, streams, reserved names, links or reparse points. No module-supplied
commands are executed by the flash backend.

`./module/tests/package.Tests.ps1` exercises isolated synthetic image sets,
cache-less builds, compiled board/project/magic/chip rejection, PSRAM/hash
rejection, exact artifact selection, archive tampering,
overwrite refusal, hidden notices and path containment. Hardware behavior is
validated separately; synthetic images are never published as firmware.
