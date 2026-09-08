#!/usr/bin/env python3
"""NS2Pro ESP32-S3 manager HID and Windows XInput command line utility."""

from __future__ import annotations

import argparse
import ctypes
import json
import sys
import time
from ctypes import wintypes

try:
    from pywinusb import hid
except ImportError:
    raise SystemExit(
        "pywinusb is required. Install it with:\n"
        "  python -m pip install pywinusb"
    )


REPORT_ID = 0x7F
SET_MAGIC = b"Y7HID1"
REPLY_MAGIC = b"Y7HRS1"
DEVICE_IDS = (
    (0x057E, 0x2069),  # Nintendo
    (0x1209, 0x4E53),  # compatible XInput + manager HID
    (0x045E, 0x028E),  # strict XInput identity
    (0x054C, 0x0CE6),  # DualSense
)


def open_manager():
    errors: list[str] = []
    for vid, pid in DEVICE_IDS:
        for device in hid.HidDeviceFilter(vendor_id=vid, product_id=pid).get_devices():
            try:
                device.open()
                reports = device.find_feature_reports()
                report = next((item for item in reports if item.report_id == REPORT_ID), None)
                if report is not None:
                    return device, report
                device.close()
            except Exception as exc:  # Continue past non-manager collections.
                errors.append(f"{vid:04x}:{pid:04x}: {exc}")
                try:
                    device.close()
                except Exception:
                    pass
    detail = "\n".join(errors[-4:])
    raise RuntimeError(f"manager HID not found\n{detail}".rstrip())


def send_command(command: str, allow_disconnect: bool = False):
    encoded = SET_MAGIC + command.strip().encode("utf-8")
    if len(encoded) > 63:
        raise ValueError("manager command is longer than 63 bytes")

    device, report = open_manager()
    try:
        report.set_raw_data([REPORT_ID, *encoded, *([0] * (63 - len(encoded)))])
        report.send()
        time.sleep(0.15)

        chunks: dict[int, bytes] = {}
        total = None
        for _ in range(64):
            try:
                raw = bytes(report.get())
            except Exception:
                if allow_disconnect:
                    return {"ok": True, "disconnected": True}
                raise
            base = 1 if raw[:1] == bytes([REPORT_ID]) else 0
            if raw[base : base + 6] != REPLY_MAGIC:
                if allow_disconnect and not raw:
                    return {"ok": True, "disconnected": True}
                raise RuntimeError(f"bad manager reply: {raw.hex()}")
            total = int.from_bytes(raw[base + 6 : base + 8], "little")
            offset = int.from_bytes(raw[base + 8 : base + 10], "little")
            size = raw[base + 10]
            chunks[offset] = raw[base + 11 : base + 11 + size]
            if sum(len(chunk) for chunk in chunks.values()) >= total:
                break
        if total is None:
            raise RuntimeError("manager returned no reply")
        payload = b"".join(chunks[key] for key in sorted(chunks))[:total]
        text = payload.decode("utf-8", errors="replace")
        try:
            return json.loads(text)
        except json.JSONDecodeError:
            return {"ok": False, "raw": text}
    finally:
        try:
            device.close()
        except Exception:
            pass


class XInputGamepad(ctypes.Structure):
    _fields_ = [
        ("buttons", wintypes.WORD),
        ("left_trigger", ctypes.c_ubyte),
        ("right_trigger", ctypes.c_ubyte),
        ("left_x", ctypes.c_short),
        ("left_y", ctypes.c_short),
        ("right_x", ctypes.c_short),
        ("right_y", ctypes.c_short),
    ]


class XInputState(ctypes.Structure):
    _fields_ = [("packet", wintypes.DWORD), ("gamepad", XInputGamepad)]


def xinput_test() -> int:
    if sys.platform != "win32":
        raise RuntimeError("XInput test is available only on Windows")
    xinput = ctypes.WinDLL("xinput1_4.dll")
    xinput.XInputGetState.argtypes = [wintypes.DWORD, ctypes.POINTER(XInputState)]
    found = False
    for index in range(4):
        state = XInputState()
        result = xinput.XInputGetState(index, ctypes.byref(state))
        if result != 0:
            continue
        found = True
        gamepad = state.gamepad
        print(json.dumps({
            "ok": True,
            "index": index,
            "packet": state.packet,
            "buttons": f"0x{gamepad.buttons:04x}",
            "triggers": [gamepad.left_trigger, gamepad.right_trigger],
            "sticks": [gamepad.left_x, gamepad.left_y, gamepad.right_x, gamepad.right_y],
        }, ensure_ascii=False, indent=2))
    if not found:
        print(json.dumps({"ok": False, "error": "no XInput controller"}, indent=2))
        return 1
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="action", required=True)
    sub.add_parser("status")
    sub.add_parser("reboot")
    sub.add_parser("xinput-test")
    mode_parser = sub.add_parser("mode")
    mode_parser.add_argument("mode", choices=("nintendo", "xinput", "dualsense"))
    mode_parser.add_argument("--reboot", action="store_true")
    command_parser = sub.add_parser("command")
    command_parser.add_argument("command")
    args = parser.parse_args()

    if args.action == "xinput-test":
        return xinput_test()
    if args.action == "status":
        result = send_command("usb status")
    elif args.action == "reboot":
        result = send_command("usb reboot", allow_disconnect=True)
    elif args.action == "mode":
        result = send_command(f"usb mode {args.mode}")
        print(json.dumps(result, ensure_ascii=False, indent=2))
        if args.reboot:
            time.sleep(0.3)
            result = send_command("usb reboot", allow_disconnect=True)
    else:
        result = send_command(args.command)
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0 if result.get("ok") else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(json.dumps({"ok": False, "error": str(exc)}, ensure_ascii=False, indent=2))
        raise SystemExit(1)
