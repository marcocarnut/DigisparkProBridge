#!/usr/bin/env python3
"""Check the bridge's RTS output: flood its UART while nobody reads its USB
port, and see whether the adapter is made to pause.

usage: rts_test.py [--adapter /dev/ttyUSB0] [--bridge /dev/ttyACM0]
                   [--baud 115200] [--seconds 10] [--no-crtscts]

The bridge's port is left closed while the data is sent, so nothing drains
what it receives: its ring fills, and it should raise RTS (PA2) and stop the
adapter. Afterwards the bridge's own counters say whether it dropped any.
"""
import argparse
import fcntl
import os
import select
import struct
import subprocess
import sys
import termios
import time
import tty


def open_adapter(path, baud, crtscts):
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    tty.setraw(fd)
    attrs = termios.tcgetattr(fd)
    attrs[4] = attrs[5] = getattr(termios, f"B{baud}")
    attrs[2] = (attrs[2] | termios.CRTSCTS) if crtscts else (attrs[2] & ~termios.CRTSCTS)
    attrs[2] |= termios.CLOCAL | termios.CREAD
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    return fd


def may_send(fd):
    """The adapter's CTS input, which is the bridge's RTS output. TIOCM_CTS is
    set when the signal is asserted, which for CTS# means the pin is low: the
    bridge saying it can take data."""
    bits = struct.unpack("I", fcntl.ioctl(fd, termios.TIOCMGET, struct.pack("I", 0)))[0]
    return bool(bits & termios.TIOCM_CTS)


def state(fd):
    return "low, may send" if may_send(fd) else "high, stop"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--adapter", default="/dev/ttyUSB0")
    ap.add_argument("--bridge", default="/dev/ttyACM0")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--seconds", type=float, default=10)
    ap.add_argument("--no-crtscts", action="store_true")
    args = ap.parse_args()

    # The bridge's UART follows the rate set on its USB port, so set it there
    # and close the port again: nothing must drain what the bridge receives.
    subprocess.run(["stty", "-F", args.bridge, str(args.baud)], check=True)
    time.sleep(0.3)
    with open(args.bridge, "rb", buffering=0) as b:  # drain what is queued
        os.set_blocking(b.fileno(), False)
        deadline = time.time() + 1
        while time.time() < deadline:
            if select.select([b.fileno()], [], [], 0.1)[0]:
                b.read(4096)
    time.sleep(0.5)

    fd = open_adapter(args.adapter, args.baud, not args.no_crtscts)
    print(f"{args.baud} bps, crtscts {'off' if args.no_crtscts else 'on'};"
          f" the bridge's RTS at rest: {state(fd)}")

    data = bytes(range(256)) * 16
    sent, end, transitions, last = 0, time.time() + args.seconds, 0, may_send(fd)
    while time.time() < end:
        now = may_send(fd)
        if now != last:
            transitions += 1
            last = now
        if select.select([], [fd], [], 0.05)[1]:
            try:
                sent += os.write(fd, data)
            except BlockingIOError:
                pass
    print(f"adapter accepted {sent} bytes in {args.seconds:g} s"
          f" ({sent / args.seconds:.0f} bytes/s)")
    print(f"the bridge's RTS now: {state(fd)}, {transitions} changes while sending")
    os.close(fd)

    # Now drain the bridge -- the counters are written to the same port, and a
    # full buffer would swallow them -- and then ask for them at 110 bps.
    bfd = os.open(args.bridge, os.O_RDONLY | os.O_NONBLOCK)
    held, deadline = 0, time.time() + 3
    while time.time() < deadline:
        if select.select([bfd], [], [], 0.3)[0]:
            held += len(os.read(bfd, 4096))
            deadline = time.time() + 1
    print(f"the bridge had {held} bytes still to hand over")
    subprocess.run(["stty", "-F", args.bridge, "110"], check=True)
    out, deadline = b"", time.time() + 2
    while time.time() < deadline and b"\n" not in out:
        if select.select([bfd], [], [], 0.1)[0]:
            out += os.read(bfd, 4096)
    os.close(bfd)
    print("bridge:", out.decode(errors="replace").strip() or "(no counters)")


if __name__ == "__main__":
    main()
