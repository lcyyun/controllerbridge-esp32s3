# ControllerBridge ESP32-S3

Experimental standalone ESP-IDF firmware:

```text
Nintendo Switch 2 Pro Controller -> BLE -> ESP32-S3 -> native USB -> host
```

## Support

Physical input and USB output are independent. The ESP32-S3 receives **NS2Pro
via Bluetooth LE**; its radio cannot receive DualSense/Edge over Bluetooth
Classic. It exposes four USB output identities: **DualSense, DualSense Edge,
XInput and NS2Pro**. Selecting a saved mode detaches USB and restarts to apply
new descriptors. The physical controller remains NS2Pro in all four modes.

- NS2Pro discovery, address/key/challenge pairing, persistent peer key, encrypted
  reconnect, one saved active peer, explicit disconnect/forget, scan retry and
  watchdog recovery. Any-button wake reconnect is implemented and awaits the
  physical power-off/wake regression described in the validation document.
- Buttons, sticks, digital NS2Pro triggers, FD2 motion, and 25 canonical mapping
  dimensions (including C/GL/GR, paddle/function and touchpad destinations).
  One NS2Pro source mapping applies to all output identities. Bindings persist
  in NVS with schema/CRC validation; unavailable source buttons remain inactive.
- Nintendo/XInput/Sony feedback translation and existing USB audio OUT to NS2Pro
  haptics. Microphone capture and a Bluetooth headset audio stream are not
  implemented. Sony console authentication is not claimed.
- Physical report rate uses FD2's 32-bit or legacy's 8-bit source counter,
  rejects duplicate/out-of-order frames, counts stationary fresh frames and
  resets on reconnect/source changes. Input older than 250 ms is neutralized.
  FD2 exposes measured battery millivolts; percentage comes only from a
  successful legacy power-characteristic read and stays unknown on failure or
  expiry. These battery reads never advance the input-frame counters.
- ControllerBridge Manager connects directly through native USB vendor HID
  feature report `0x7e` in every mode. `manager_protocol.cpp` is compiled and
  carries bounded fragmented NSM1 commands. BLE/NVS work executes outside USB
  callbacks. Existing Y7HID1/Y7HRS1 commands remain usable.
- Status, input/motion diagnostics, mapping, settings save/reset, pairing,
  rumble tests, and reboot are available in Manager. ROM recovery uses the
  physical BOOT + RESET procedure and the board's flashing serial port.

No hardware was flashed during this delivery. Host tests and ESP-IDF builds
are distinct from controller, host-driver, audio and board validation. See
`tools/tests/BLE-INPUT-VALIDATION.md` for the physical regression checklist.

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

It honors `IDF_PATH`, `IDF_TOOLS_PATH`, and `IDF_PYTHON_ENV_PATH`. It next checks
repository-local `.cache/esp-idf-v5.3.3` and `.cache/idf-tools`. Other fallback
paths are `C:\Espressif\frameworks\esp-idf-v5.3.3`,
`$env:USERPROFILE\.espressif`, and its `python_env\idf5.3_py3.12_env`.
Tools are selected by that IDF installation's `idf_tools.py export`; the
wrapper does not install or upgrade them. Build commands do not flash a device.

The GitHub Actions workflow is manual (`workflow_dispatch`) and build-only.
It uses a pinned IDF 5.3.3 container, checks that the committed dependency lock
does not change, and stores build artifacts for seven days. It does not flash,
create releases, or publish firmware automatically.

## Manager modules and firmware recovery

Build the exact memory profile, then package its complete image set:

```powershell
./module/package.ps1 -BuildDirectories ./build-n16r8
# Multiple independently built profiles can share one module:
./module/package.ps1 -BuildDirectories ./build-n4,./build-n16r8
./tools/package-release.ps1 -BuildDirectory ./build-n16r8 -Board n16r8
```

The `.cbmodule` includes bootloader at `0x0`, partition table at `0x8000`,
application at `0x10000`, a board-specific flash manifest and payload hashes.
Only explicitly supplied and validated board builds are advertised as
installable. Manager's EspressifSerial backend uses its bundled esptool,
probes ESP32-S3/flash capacity before writing and verifies the images after
writing; no separate Python Manager process is needed. This requires the
matching Manager integration patch delivered with this firmware preview.

For native USB HID, connect the USB data connector wired to GPIO19/20.
The USB-to-UART connector is a separate flashing/logging interface. Hold BOOT,
press/release RESET, then release BOOT to enter ROM download mode if automatic
serial reset is unavailable. Select the ESP32-S3 serial port, not an unrelated
RP2350 serial bridge. The packaging/build/test commands never flash hardware.

Legacy optional tools remain available (`ns2-s3-manager.py` with pywinusb).
They are for diagnostics and are not required for Manager functionality.

## Offline tests

```powershell
./tools/test-input.ps1
./tools/test-mapping.ps1
./tools/test-manager-protocol.ps1
./tools/test-package-release.ps1
./module/tests/package.Tests.ps1
python ./tools/test_usb_contract.py --compiler g++
```

C++ host tests accept `-Compiler` for a C++17 compiler. They exercise the
actual parser, mapping/codecs and Manager transport implementation. Packaging
tests use synthetic fixtures and must not be mistaken for board tests.

## Provenance

`SOURCE-SNAPSHOT.json` preserves the original import paths and SHA256 values.
It is an import record, not an attestation that edited files still match or
that firmware was hardware-tested. Preserve `LICENSE`, `NOTICE.md`, and
`LICENSES/`; the imported notice includes historical monorepo references.
