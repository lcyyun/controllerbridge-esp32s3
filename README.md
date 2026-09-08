# ControllerBridge ESP32-S3

Experimental standalone ESP-IDF firmware:

```text
Nintendo Switch 2 Pro Controller -> BLE -> ESP32-S3 -> native USB -> host
```

## Support

- NS2Pro BLE discovery, connection, GATT initialization, input notifications,
  reconnect handling, and NVS-backed settings.
- Nintendo-style HID, compatible XInput, and DualSense-style USB output modes.
  These are output identities, not support for receiving Xbox or DualSense
  controllers over Bluetooth.
- Input forwarding and rumble translation. The DualSense output path also has
  experimental USB audio-OUT-to-haptics handling; microphone streaming is not
  implemented.
- Legacy HID feature commands in `main/ns2_usb.cpp`, used by
  `ns2-s3-manager.py`. The separate `main/manager_protocol.*` source is retained
  but is not compiled or integrated.

The imported README recorded tests with one controller setup. This split does
not establish additional controller, console, host-driver, audio, or board
compatibility. Security/bond recovery, rumble quality, and long-running
reconnection still need hardware validation.

## Boards

All profiles require an ESP32-S3 with its native USB data pins connected to the
host-facing USB connector. A USB-to-serial connector alone cannot provide the
controller USB interface. Select the actual flash/PSRAM configuration:

| Profile | Flash | PSRAM |
| --- | --- | --- |
| `n4` | 4 MB | Disabled |
| `n8` | 8 MB | Disabled |
| `n8r2` | 8 MB | 2 MB Quad SPI |
| `n8r8` | 8 MB | 8 MB Octal SPI |
| `n16r8` (default) | 16 MB | 8 MB Octal SPI |
| `esp32supermini` | 4 MB | 2 MB Quad SPI |

Profiles specify memory settings, not a guarantee for every board sold under
those names. BOOT defaults to GPIO0 and the addressable status LED to GPIO48;
adjust or disable the LED in `idf.py menuconfig` for different wiring.
Double-clicking BOOT cycles the saved USB output mode and reboots.

## Build

Run from this repository root. Use **ESP-IDF 5.3.3**, with its installed Python
environment and tools. The manifest and tracked `dependencies.lock` pin:

| Dependency | Version |
| --- | --- |
| ESP-IDF | `5.3.3` |
| `espressif/esp_tinyusb` | `2.2.0` |
| `espressif/tinyusb` | `0.19.0~3` |
| `espressif/led_strip` | `2.5.5` |

With the IDF environment activated:

```sh
idf.py -B build-n16r8 -DNS2_ESP32S3_BOARD=n16r8 build
idf.py -B build-n4 -DNS2_ESP32S3_BOARD=n4 build
```

Use a distinct build directory per profile. Generated `sdkconfig`, binaries,
and logs stay in that directory; downloaded components go in
`managed_components/`. Keep `dependencies.lock` in version control.

The Windows wrapper resolves the project relative to the script, so it also
works when invoked from another current directory:

```powershell
.\ns2-s3.ps1 build -Board n16r8
.\ns2-s3.ps1 build -Board esp32supermini -BuildDir build-supermini
```

It honors `IDF_PATH`, `IDF_TOOLS_PATH`, and `IDF_PYTHON_ENV_PATH`. Local fallback
paths are `C:\Espressif\frameworks\esp-idf-v5.3.3`,
`$env:USERPROFILE\.espressif`, and its `python_env\idf5.3_py3.12_env`.
Tools are selected by that IDF installation's `idf_tools.py export`; the
wrapper does not install or upgrade them. Build commands do not flash a device.

The GitHub Actions workflow is manual (`workflow_dispatch`) and build-only.
It uses a pinned IDF 5.3.3 container, checks that the committed dependency lock
does not change, and stores build artifacts for seven days. It does not flash,
create releases, or publish firmware automatically.

The standalone N16R8 build has passed locally with the pinned toolchain.
Known non-fatal warnings include the imported, now-ignored
`TINYUSB_TASK_STACK_SIZE` setting, an unused input helper, and Windows object
path length warnings for long checkout paths. No runtime stack-size change was
made during the split; validate hardware behavior separately.

## Optional Device Tools

On Windows, `ns2-s3-manager.py` provides legacy HID status, mode, reboot, and
raw-command operations, plus an XInput read check. It requires `pywinusb` in
the Python environment used to run it. Do not assume a connected device
supports the separate dormant manager protocol.

```powershell
python .\ns2-s3-manager.py status
python .\ns2-s3-manager.py xinput-test
```

The PowerShell wrapper also retains explicit `flash`, `build-flash`, and
`monitor` actions with `-Port`; these are not part of a build-only check.

## Provenance

`SOURCE-SNAPSHOT.json` preserves the original import paths and SHA256 values.
It is an import record, not an attestation that edited files still match or
that firmware was hardware-tested. Preserve `LICENSE`, `NOTICE.md`, and
`LICENSES/`; the imported notice includes historical monorepo references.
