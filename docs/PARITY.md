# ESP32-S3 parity implementation and verification

Version: 0.2.0-preview.1. Reference: ControllerBridge BL616 0.2.3-preview.1.
This matrix describes source implementation. Hardware results are explicitly separate.

| Area | ESP implementation | Software verification | Hardware scope |
|---|---|---|---|
| Physical input | One NS2Pro over BLE; saved-peer matching, explicit pairing/forget | Parser/GATT/pairing vectors and reconnect policy tests | Pairing and RF not tested |
| DS5/Edge Bluetooth input | Unavailable: ESP32-S3 has BLE, no Bluetooth Classic radio | No unsupported identity advertised | Physical radio limitation |
| USB output | Nintendo NS2Pro, XInput, DualSense, DualSense Edge independent of physical input | Report/mapping/calibration/Edge codec tests and compiled descriptors | Windows/game/console compatibility not tested; Sony authentication not claimed |
| Switching | Save requested role, return response, detach and deferred restart | Mode validation/dispatch tests | USB re-enumeration and driver rebinding not tested |
| Reconnect | NS2 address/key/challenge, AES verification, key NVS; encrypted saved-peer reconnect; retry scans after failure | Published crypto vector, malformed ACKs, saved-peer policies, host callback audit | Connection-key off then arbitrary-button wake is a required physical regression |
| Persistence/recovery | Settings NVS, 25-key mapping SFM1 CRC, saved BLE secret; reset waits for key/bond erase | Mapping save/reload/corrupt/failure tests, async control review | Power-cut durability not tested |
| Mapping | One canonical NS2Pro source profile, 25 source/destination names including C/GL/GR and Edge destinations | Every binding name, defaults, remap/disable, trigger conversion and nonidentity raw invalidation | Button feel/order not tested |
| Input/motion | FD2 buttons/12-bit axes/digital triggers; real FD2 motion timestamp, accel and gyro; neutral after 250ms | Captured-format parsing, stale/reset, Sony motion field tests | Calibration/axis orientation needs controller test |
| Report rate | Real FD2 LE32 counter; legacy uint8 counter codec; wrap-aware dedup; source epoch and stale clear | Stationary new frames, duplicate/out-of-order, wrap, reconnect/source switch tests; Manager rate/vector smoke | RF rate distributions not measured |
| Battery | FD2 measured voltage; periodic legacy power READ when supported; unknown on failure/expiry | Independent battery cache tests prove no input count/epoch/liveness change | Legacy characteristic read behavior not verified on device |
| Rumble/audio | Existing Nintendo/XInput/Sony rumble translation; Sony USB audio OUT to NS2 haptics; stale/source transition clears feedback | Codec/queue source checks; DSP path compiled | Rumble fidelity, audio timing, headset behavior not tested; microphone streaming not implemented |
| Manager protocol | NSM1 feature0x7e in the existing vendor HID collection in every output role; old Y7 retained; bounded fragmented commands deferred to app task | Actual protocol code: fragmentation, malformed packets, Busy, in-flight/unread response protection, 4 mode commands | HidSharp device enumeration/traffic not tested against physical ESP |
| Firmware update | Board-specific bootloader/partition/application and complete SHA inventory; fixed EspressifSerial plan | Raw package + module tamper/path/board/chip tests; Manager strict plan, probe/write/verify tests with fake process | No serial port/device flashed |
| Recovery transport | ROM loader through flashing COM interface; physical BOOT+RESET fallback | Bundled official esptool and fixed arguments | HID command to enter ROM is not implemented; no automatic remote-ROM claim |

The default board is N16R8. Source profiles also retain n4, n8, n8r2, n8r8
and esp32supermini. A published module contains only the individually built
profiles listed in its firmware entries. Its image is never reused under
another memory profile. ROM can probe flash capacity; PSRAM mode and board
wiring require correct user selection.

Protocol evidence: the NS2 report layout is documented at
https://github.com/ndeadly/switch2_controller_research/blob/master/hid_reports.md.
ESP-IDF 5.3.3 source and pinned NimBLE/TinyUSB headers define the stack API;
no APIs from another SDK generation are assumed. See
`tools/tests/BLE-INPUT-VALIDATION.md` for deterministic vectors and hardware steps.
