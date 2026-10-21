#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""
Bring-up / debugging tool for the WHB04 Network Bridge.

Connects to a bridge, prints the handshake and every decoded pendant event
(buttons, axis and step selector, jog wheel), and keeps the connection alive
with heartbeats. With --display it also writes test values to the pendant's
LCD so the feature-report path can be checked.

Only uses the Python standard library:

    python tools/bridge_monitor.py 192.168.1.50
    python tools/bridge_monitor.py whb04-bridge.local:5104 --display
"""

from __future__ import annotations

import argparse
import select
import socket
import struct
import sys
import time

DEFAULT_PORT = 5104

FRAME_HELLO = 0x01
FRAME_STATUS = 0x02
FRAME_INPUT_REPORT = 0x03
FRAME_HEARTBEAT = 0x04
FRAME_FEATURE_REPORT = 0x81
FRAME_CLIENT_HEARTBEAT = 0x82

BUTTONS = {
    0x01: "RESET",
    0x02: "STOP",
    0x03: "START/PAUSE",
    0x04: "FEED+",
    0x05: "FEED-",
    0x06: "SPINDLE+",
    0x07: "SPINDLE-",
    0x08: "M-HOME",
    0x09: "SAFE-Z",
    0x0A: "W-HOME",
    0x0B: "S-ON/OFF",
    0x0C: "FN",
    0x0D: "PROBE-Z",
    0x0E: "CONTINUOUS",
    0x0F: "STEP",
    0x10: "MACRO-10",
}
AXES = {0x06: "OFF", 0x11: "X", 0x12: "Y", 0x13: "Z", 0x14: "A", 0x15: "B", 0x16: "C"}
STEPS = {0x0D: "0.001", 0x0E: "0.01", 0x0F: "0.1", 0x10: "1", 0x1A: "60%", 0x1B: "100%", 0x9B: "Lead"}


def parse_address(value: str) -> tuple[str, int]:
    if value.count(":") == 1:
        host, port = value.split(":")
        return host, int(port)
    return value, DEFAULT_PORT


def frame(frame_type: int, payload: bytes = b"") -> bytes:
    return bytes((frame_type, len(payload))) + payload


def display_frames(x: float, y: float, z: float, feed: int, spindle: int) -> bytes:
    """Builds the three 0x06 feature reports the WHB04 LCD expects (step mode, machine coords)."""
    data = struct.pack("<HBB", 0xFDFE, 0xFE, 0x01)
    for coord in (x, y, z):
        scaled = int(round(abs(coord) * 10000))
        data += struct.pack("<HH", scaled // 10000, (scaled % 10000) | ((1 if coord < 0 else 0) << 15))
    data += struct.pack("<HH", feed & 0xFFFF, spindle & 0xFFFF)
    data = data.ljust(21, b"\x00")
    return b"".join(frame(FRAME_FEATURE_REPORT, b"\x06" + data[i : i + 7]) for i in range(0, 21, 7))


def describe_report(report: bytes, last: dict) -> str | None:
    if len(report) != 8 or report[0] != 0x04:
        return f"raw report: {report.hex(' ')}"
    _, _, b1, b2, step, axis, jog, _ = struct.unpack("BBBBBBbB", report)
    parts = []
    buttons = sorted({b for b in (b1, b2) if b})
    if buttons != last.get("buttons"):
        names = [BUTTONS.get(b, f"0x{b:02X}") for b in buttons]
        parts.append("buttons: " + (", ".join(names) if names else "none"))
        last["buttons"] = buttons
    if axis != last.get("axis"):
        parts.append(f"axis: {AXES.get(axis, f'0x{axis:02X}')}")
        last["axis"] = axis
    if step != last.get("step"):
        parts.append(f"step: {STEPS.get(step, f'0x{step:02X}')}")
        last["step"] = step
    if jog:
        parts.append(f"jog: {jog:+d}")
    return "  ".join(parts) if parts else None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("address", help="bridge host or host:port")
    parser.add_argument("--display", action="store_true", help="write test values to the pendant LCD")
    parser.add_argument("--raw", action="store_true", help="also print raw report bytes")
    args = parser.parse_args()

    host, port = parse_address(args.address)
    print(f"Connecting to {host}:{port} ...")
    sock = socket.create_connection((host, port), timeout=5)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    sock.setblocking(False)

    buffer = bytearray()
    last_rx = last_tx = time.monotonic()
    last_display = 0.0
    jog_total = 0
    state: dict = {}

    try:
        while True:
            now = time.monotonic()
            if now - last_tx > 0.5:
                sock.sendall(frame(FRAME_CLIENT_HEARTBEAT))
                last_tx = now
            if args.display and now - last_display > 0.5:
                sock.sendall(display_frames(jog_total * 0.1, -12.3456, 7.5, 1234, 12000))
                last_display = last_tx = now
            if now - last_rx > 3:
                print("Bridge stopped responding")
                return 1

            readable, _, _ = select.select([sock], [], [], 0.1)
            if not readable:
                continue
            chunk = sock.recv(4096)
            if not chunk:
                print("Bridge closed the connection (another client may have connected)")
                return 1
            last_rx = time.monotonic()
            buffer.extend(chunk)

            while len(buffer) >= 2 and len(buffer) >= 2 + buffer[1]:
                frame_type, length = buffer[0], buffer[1]
                payload = bytes(buffer[2 : 2 + length])
                del buffer[: 2 + length]
                stamp = time.strftime("%H:%M:%S")
                if frame_type == FRAME_HELLO:
                    print(f"{stamp} HELLO protocol v{payload[0]}, firmware {payload[1:].decode(errors='replace')}")
                elif frame_type == FRAME_STATUS:
                    attached = bool(payload and payload[0])
                    print(f"{stamp} dongle {'ATTACHED' if attached else 'NOT ATTACHED'}")
                elif frame_type == FRAME_INPUT_REPORT:
                    if len(payload) == 8 and payload[0] == 0x04:
                        jog_total += struct.unpack("b", payload[6:7])[0]
                    text = describe_report(payload, state)
                    if args.raw:
                        print(f"{stamp} {payload.hex(' ')}  {text or ''}")
                    elif text:
                        print(f"{stamp} {text}")
                elif frame_type == FRAME_HEARTBEAT:
                    pass
                else:
                    print(f"{stamp} unknown frame 0x{frame_type:02X}: {payload.hex(' ')}")
    except KeyboardInterrupt:
        return 0
    finally:
        sock.close()


if __name__ == "__main__":
    sys.exit(main())
