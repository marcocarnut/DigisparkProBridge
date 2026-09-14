#!/usr/bin/env python3
"""Drive LinDuplex: the adapter sends a counting sequence to the board's UART
for a while with the board's transmitter off, then on, and prints the board's
reports plus what the adapter received.

usage: lin_duplex_test.py [BAUD] [SECONDS_EACH]   (defaults 57600, 8)
"""
import os
import re
import select
import sys
import termios
import time
import tty

BRIDGE, ADAPTER = "/dev/ttyACM0", "/dev/ttyUSB0"


def open_port(path, baud):
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    tty.setraw(fd)
    attrs = termios.tcgetattr(fd)
    attrs[4] = attrs[5] = getattr(termios, f"B{baud}")
    attrs[2] |= termios.CLOCAL | termios.CREAD
    attrs[2] &= ~termios.CRTSCTS
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    return fd


def main():
    baud = int(sys.argv[1]) if len(sys.argv) > 1 else 57600
    seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 8
    board = open_port(BRIDGE, baud)
    adapter = open_port(ADAPTER, baud)
    time.sleep(0.5)
    termios.tcflush(adapter, termios.TCIOFLUSH)
    counter = 0
    for phase in ("board transmitter off", "board transmitter on"):
        if phase.endswith("on"):
            os.write(board, b"t")
        print(f"== {baud} bps, {phase}")
        reports, got = b"", b""
        end = time.time() + seconds
        while time.time() < end:
            readable, writable, _ = select.select([board, adapter], [adapter], [], 0.05)
            if adapter in writable:
                chunk = bytes((counter + i) & 0xFF for i in range(64))
                try:
                    counter += os.write(adapter, chunk)
                except BlockingIOError:
                    pass
            if board in readable:
                reports += os.read(board, 4096)
            if adapter in readable:
                got += os.read(adapter, 4096)
        lines = [l for l in reports.decode(errors="replace").splitlines() if l.startswith("rx")]
        for line in lines[1:]:  # the first report may cover the phase change
            print("  board:", line)
        breaks = sum(1 for a, b in zip(got, got[1:]) if b != (a + 1) & 0xFF)
        print(f"  adapter received {len(got)} bytes from the board, {breaks} sequence breaks")
    os.write(board, b"t")
    os.close(board)
    os.close(adapter)


if __name__ == "__main__":
    main()
