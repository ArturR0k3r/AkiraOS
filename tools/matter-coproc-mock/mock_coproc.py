#!/usr/bin/env python3
# Copyright (c) 2026 PenEngineering S.R.L
# SPDX-License-Identifier: Apache-2.0
"""Host-side mock Matter co-processor for AkiraOS bring-up.

Speaks the AkiraOS Matter co-processor IPC framing over a serial port (or PTY)
so the AkiraOS-side accessory path can be exercised against real hardware
without a physical esp-matter co-processor. This is ALSO the executable
contract that the real co-processor firmware (tools/matter-coproc/, separate
track) must implement.

Frame layout (all multi-byte fields big-endian):
    [0xAC][0xCE][CMD:1][SEQ:1][LEN_H:1][LEN_L:1][PAYLOAD:LEN][CRC16_H:1][CRC16_L:1]
CRC is CRC-16/IBM (poly 0x8005, init 0xFFFF, reflect in/out, xorout 0xFFFF).

Usage:
    python3 mock_coproc.py /dev/ttyUSB1            # talk to a real UART
    python3 mock_coproc.py --pty                   # create a PTY and print its path
"""
import argparse
import sys
import time

SYNC0, SYNC1 = 0xAC, 0xCE

# Request / response command bytes (see src/runtime/akira_matter_ipc.h).
CMD_COMMISSION_REQ = 0x01
CMD_SEND_REQ = 0x02
CMD_SUBSCRIBE_REQ = 0x03
CMD_EVENT = 0x04
CMD_STATUS_REQ = 0x05
CMD_EP_ADD_REQ = 0x06
CMD_ATTR_REPORT_REQ = 0x07
CMD_PAIR_OPEN_REQ = 0x08
CMD_QR_GET_REQ = 0x09
CMD_ACC_CMD_EVENT = 0x0A

MOCK_QR = b"MT:MOCK.AKIRA000MATTER01\x00"
MOCK_MANUAL = b"3497-011-2332\x00"


def crc16_ibm(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ 0xA001  # reflected 0x8005
            else:
                crc >>= 1
    return crc ^ 0xFFFF


def build_frame(cmd: int, seq: int, payload: bytes) -> bytes:
    hdr = bytes([SYNC0, SYNC1, cmd, seq, len(payload) >> 8, len(payload) & 0xFF])
    crc = crc16_ibm(hdr + payload)
    return hdr + payload + bytes([crc >> 8, crc & 0xFF])


def status_payload(status: int, extra: bytes = b"") -> bytes:
    return status.to_bytes(4, "big", signed=True) + extra


class MockCoproc:
    def __init__(self, write):
        self._write = write
        self._next_ep = 1

    def handle(self, cmd: int, seq: int, payload: bytes):
        if cmd in (CMD_STATUS_REQ, CMD_SEND_REQ, CMD_SUBSCRIBE_REQ,
                   CMD_ATTR_REPORT_REQ, CMD_PAIR_OPEN_REQ):
            self._respond(cmd, seq, status_payload(0))
        elif cmd == CMD_COMMISSION_REQ:
            eui64 = bytes([0x02, 0, 0, 0, 0, 0, 0, 1])
            self._respond(cmd, seq, status_payload(0, eui64))
        elif cmd == CMD_EP_ADD_REQ:
            ep = self._next_ep
            self._next_ep += 1
            self._respond(cmd, seq, status_payload(0, bytes([ep])))
            print(f"[mock] endpoint {ep} registered; injecting ON", flush=True)
            self.inject_command(ep, 0x0006, 0x0001)  # OnOff / On
        elif cmd == CMD_QR_GET_REQ:
            self._respond(cmd, seq, status_payload(0, MOCK_QR + MOCK_MANUAL))
        else:
            print(f"[mock] unknown cmd 0x{cmd:02x}", flush=True)
            self._respond(cmd, seq, status_payload(-3))

    def inject_command(self, endpoint: int, cluster: int, cmd: int,
                       value: bytes = b""):
        payload = (bytes([endpoint]) + cluster.to_bytes(4, "big") +
                   cmd.to_bytes(4, "big") + value)
        self._write(build_frame(CMD_ACC_CMD_EVENT, 0, payload))

    def _respond(self, req_cmd: int, seq: int, payload: bytes):
        self._write(build_frame(req_cmd | 0x80, seq, payload))


def parse_stream(byte_iter, coproc: "MockCoproc"):
    """Drive the mock from a byte iterator (blocking)."""
    state = 0
    cmd = seq = 0
    length = 0
    got = bytearray()
    hdr = bytearray()
    crc_recv = 0
    for b in byte_iter:
        if state == 0:
            if b == SYNC0:
                state = 1
        elif state == 1:
            state = 2 if b == SYNC1 else 0
        elif state == 2:
            cmd = b
            hdr = bytearray([SYNC0, SYNC1, b])
            state = 3
        elif state == 3:
            seq = b
            hdr.append(b)
            state = 4
        elif state == 4:
            length = b << 8
            hdr.append(b)
            state = 5
        elif state == 5:
            length |= b
            hdr.append(b)
            got = bytearray()
            state = 6 if length > 0 else 7
        elif state == 6:
            got.append(b)
            if len(got) >= length:
                state = 7
        elif state == 7:
            crc_recv = b << 8
            state = 8
        elif state == 8:
            crc_recv |= b
            if crc16_ibm(bytes(hdr) + bytes(got)) == crc_recv:
                coproc.handle(cmd, seq, bytes(got))
            else:
                print("[mock] CRC mismatch", flush=True)
            state = 0


def main():
    ap = argparse.ArgumentParser(description="Mock AkiraOS Matter co-processor")
    ap.add_argument("port", nargs="?", help="serial device (e.g. /dev/ttyUSB1)")
    ap.add_argument("--pty", action="store_true",
                    help="create a PTY instead of opening a serial device")
    ap.add_argument("--baud", type=int, default=115200)
    args = ap.parse_args()

    if args.pty:
        import os, pty
        primary, secondary = pty.openpty()
        print(f"[mock] PTY ready: {os.ttyname(secondary)}", flush=True)

        def write(data):
            os.write(primary, data)

        coproc = MockCoproc(write)

        def reader():
            while True:
                chunk = os.read(primary, 256)
                if not chunk:
                    return
                yield from chunk

        parse_stream(reader(), coproc)
        return

    if not args.port:
        ap.error("a serial port is required unless --pty is given")

    try:
        import serial  # pyserial
    except ImportError:
        sys.exit("pyserial not installed: pip install pyserial (or use --pty)")

    ser = serial.Serial(args.port, args.baud, timeout=0.05)
    print(f"[mock] listening on {args.port} @ {args.baud}", flush=True)

    def write(data):
        ser.write(data)

    coproc = MockCoproc(write)

    def reader():
        while True:
            chunk = ser.read(256)
            if chunk:
                yield from chunk
            else:
                time.sleep(0.005)

    parse_stream(reader(), coproc)


if __name__ == "__main__":
    main()
