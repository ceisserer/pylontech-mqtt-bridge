#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Pylontech battery emulator for testing pylon2mqtt without hardware.

Answers the commands used by pylon2mqtt / python-pylontech with the
payloads recorded from a real US3000C. Listens on TCP (use
"port": "socket://127.0.0.1:<port>") or on a pseudo terminal (--pty,
prints the device path to use as "port").

Usage: pylon_emulator.py [--tcp PORT | --pty] [--batteries N] [--current A]
"""
import argparse
import os
import pty
import socket
import sys
import tty

SERIAL = "Y221006C50140020"
ANALOG = ("00020F0C4E0C3F0C5E0C510C570C580C540C4C0C5B0C4A0C550C610C4E0C580C4F"
          "060B940B8D0B8E0B8B0B8A0B8D0000B8DBFFFF04FFFF02610016080186A0")
CHARGE = "02D002AFC80320FCE0C0"
ALARM = "00020F00000000000000000000000000000006000000000000000000000E00000000"
SYSTEM = "100E420BEA0AF00D030A4703E8D2F0B3B0A7F80D030A47FC18"


def checksum(body):
    return (~sum(body.encode()) + 1) & 0xFFFF


def length_field(lenid):
    cs = (lenid & 0xF) + ((lenid >> 4) & 0xF) + ((lenid >> 8) & 0xF)
    cs = ((~cs) & 0xF) + 1
    return ((cs << 12) + lenid) & 0xFFFF


def response(adr, payload):
    body = "20%02X4600%04X%s" % (adr, length_field(len(payload)), payload)
    return ("~%s%04X\r" % (body, checksum(body))).encode()


def answer(frame, args):
    """frame: command without ~ and \\r; returns the response or None"""
    body, chk = frame[:-4], int(frame[-4:], 16)
    if checksum(body) != chk:
        print("checksum error:", frame, file=sys.stderr)
        return None
    adr, cid2 = int(body[2:4], 16), int(body[6:8], 16)
    batt = adr - 2
    if batt < 0 or batt >= args.batteries:
        return None
    if cid2 == 0x93:
        return response(adr, "02" + (SERIAL[:-1] + "%X" % batt).encode().hex().upper())
    if cid2 == 0x42:
        # battery b: first cell + b mV, configurable current
        p = ANALOG[:6] + "%04X" % (0x0C4E + batt) + ANALOG[10:92] + \
            "%04X" % (int(round(args.current * 10)) & 0xFFFF) + ANALOG[96:]
        return response(adr, p)
    if cid2 == 0x92:
        return response(adr, CHARGE)
    if cid2 == 0x44:
        return response(adr, ALARM)
    if cid2 == 0x47:
        return response(adr, SYSTEM)
    return None


def serve(read, write, args):
    buf = b""
    while True:
        data = read()
        if not data:
            return
        buf += data
        while b"\r" in buf:
            frame, buf = buf.split(b"\r", 1)
            if b"~" not in frame:
                continue
            frame = frame[frame.rindex(b"~") + 1:].decode()
            if args.verbose:
                print("->", frame, file=sys.stderr)
            resp = answer(frame, args)
            if resp:
                if args.verbose:
                    print("<-", resp.decode().strip(), file=sys.stderr)
                write(resp)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tcp", type=int, default=0)
    ap.add_argument("--pty", action="store_true")
    ap.add_argument("--batteries", type=int, default=1)
    ap.add_argument("--current", type=float, default=0.0)
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    if args.pty:
        master, slave = pty.openpty()
        tty.setraw(slave)
        print(os.ttyname(slave), flush=True)
        serve(lambda: os.read(master, 1024), lambda d: os.write(master, d), args)
        return

    srv = socket.create_server(("127.0.0.1", args.tcp))
    print("socket://127.0.0.1:%d" % srv.getsockname()[1], flush=True)
    while True:
        conn, _ = srv.accept()
        with conn:
            try:
                serve(lambda: conn.recv(1024), conn.sendall, args)
            except OSError:
                pass


if __name__ == "__main__":
    main()
