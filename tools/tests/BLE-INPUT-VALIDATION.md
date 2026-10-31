# BLE input and reconnect validation

Run `tools/test-input.ps1` with a host C++17 compiler. It compiles the actual
`ns2_input.cpp`, `ns2_gatt.cpp`, and `ns2_protocol.cpp` with warnings as errors.
The ESP timer is the only parser dependency replaced by a deterministic test
clock. No controller or serial port is opened.

The assertions cover stationary frames whose source counter advances,
duplicate counters with changed button payload, reordered counters, 32-bit and
8-bit wrap, reconnect reset, report-kind switch, stale neutralization, unknown
and malformed reports, extension buttons, motion, battery voltage and legacy
power encodings. They also cover pairing command vectors, truncated or rejected
ACKs, corrupt/invalid persisted record formats, saved-peer admission, explicit
disconnect versus scan, and capped reconnect retry delays.

FD2 telemetry uses the controller's little-endian 32-bit counter at bytes
0..3. Legacy 0x09 telemetry uses its 8-bit counter at byte 0. The local receive
clock is used only for measurement intervals and stale timeout. FD2 byte 3 is
not battery data. FD2 exposes measured millivolts at 0x1f..0x20 and keeps battery
percentage unknown unless a separate read of the real legacy characteristic
succeeds. Legacy byte 1 exposes the defined 0..9 power level and charging bit.
The runtime reads that attribute at most every ten seconds and only when its
discovered GATT properties allow READ. Its percentage has an independent
30-second expiry; failures make it unknown. This side channel never increments
the input sample counter, changes input source, or revives stale controls.
Source counters are not inferred from button changes or USB transmissions.

Protocol layout evidence: [Nintendo controller research HID reports](https://github.com/ndeadly/switch2_controller_research/blob/master/hid_reports.md).
The existing BL616 `ble_gatt_bl616.c` and `bl616_ns2_gatt.c` pairing sequence was
adapted to NimBLE. ESP-IDF 5.3.3's pinned NimBLE `ble_gap_security_initiate` reads
the peer security store and starts encryption with an existing LTK. The ESP
adapter delegates unrelated security-store reads to the original store hook.
The key, random contributions, and AES challenge material are never returned
by management telemetry or printed in logs.

Factory recovery does not require a running BLE host. When BLE initialization
failed, the application task clears the saved target, the `ns2_pair` key record,
and only the pinned NimBLE `nimble_bond` namespace. It never erases the NVS
partition. If a host exists but has not synchronized, its worker can still
process Forget. Completion waits for successful durable erasure; pending writes
and storage failures block the subsequent factory-reset reboot. Host policy
tests cover pending/error/remaining-key states and successful no-host completion.
With a running host, Forget blocks new connections, waits for pending connection
cancellation and disconnection, clears the SDK store RAM, then durably erases the
same namespace. Its separate bond error survives unrelated settings saves; a
retry clears the namespace even when the remembered address is already gone.

BLE controls, scan/retry, GATT progression, and rumble are serialized on the
NimBLE host event queue. NVS commits run from the application task. The runtime
rejects queued rumble when the source epoch changes or input becomes stale. It
requires NS2Pro's rumble UUID as well as command, ACK, and input UUIDs before
initialization; it no longer fabricates successful discovery with fixed
handles. The known NS2Pro CCCD immediately after each notification value is
used, matching the referenced layout. Real GATT layout variants still require
hardware checks.

These host checks do not establish RF pairing, encryption, wake/reconnect,
controller firmware compatibility, NVS power-loss recovery, latency, rumble,
audio, or USB-host behavior. Regress the connection-button power-off followed
by any-button wake, automatic reconnect after a failed scan/connect/ACK, manual
disconnect, pair replacement, cold boot, clear pairing/factory reset, battery
states, and sustained motion on real N16R8 hardware before marking those items
hardware verified.
