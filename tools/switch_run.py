#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""A build run on the Switch through the receiver (port/switch/receiver.cpp), with nobody there.

    python3 tools/switch_run.py NRO LOG SECONDS [ARGS]

The receiver must be waiting on the Switch: started once from the homebrew menu ("sly1-nx
receiver"), or started again by the last run's end. To put it there the first time, or a new one:
    nxlink -a ADDRESS build-switch/sly1_receiver.nro        (the menu waiting, Y)
or, through a receiver already waiting:
    python3 tools/switch_run.py build-switch/sly1_receiver.nro /dev/null 5
It copies itself to sdmc:/switch/sly1_receiver/, where the menu lists it. The build is sent with ARGS and with SLY1_NEXT, so that it starts the
receiver again when it ends, and SLY1_DEADLINE, so that a run that hangs is ended; its output comes
back as under nxlink (port 28771) and goes to LOG until it ends or SECONDS pass.

The address is SLY1_SWITCH's (192.168.86.35). Exit status: 0 the run ended and said so, 1 it did
not say so within SECONDS, 2 the receiver was not reached or refused the build.
"""
import os
import socket
import struct
import sys
import time

RECEIVER_PORT, LOG_PORT = 28299, 28771
RECEIVER_PATH = "sdmc:/switch/sly1_receiver/sly1_receiver.nro"


def send(address, nro, args, wait=180.0):
    """The build sent to the receiver, waited for up to `wait` seconds (it may be starting again)."""
    data = open(nro, "rb").read()
    end = time.time() + wait
    last = "not tried"
    while time.time() < end:
        try:
            with socket.create_connection((address, RECEIVER_PORT), timeout=5) as s:
                s.settimeout(60)
                a = args.encode()
                s.sendall(b"SLY1RX1\n" + struct.pack("<I", len(a)) + a + struct.pack("<Q", len(data)))
                s.sendall(data)
                answer = b""
                while not answer.endswith(b"\n"):
                    got = s.recv(256)
                    if not got:
                        break
                    answer += got
                if answer == b"OK\n":
                    return None
                return "the receiver answered: " + answer.decode("latin1").strip()
        except OSError as e:
            last = str(e)
            time.sleep(2)
    return "the receiver was not reached in %.0f s (%s)" % (wait, last)


def run(nro, log, seconds, args, address=None, echo=None):
    address = address or os.environ.get("SLY1_SWITCH", "192.168.86.35")
    args = ("%s --env SLY1_NEXT=%s --env SLY1_DEADLINE=%d" % (args, RECEIVER_PATH, int(seconds))).strip()
    listener = socket.socket()
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("", LOG_PORT))
    listener.listen(1)
    try:
        failed = send(address, nro, args)
        if failed:
            print("switch_run: " + failed, file=sys.stderr)
            return 2
        listener.settimeout(60)
        try:
            link, _ = listener.accept()
        except socket.timeout:
            print("switch_run: the build did not connect back within 60 s", file=sys.stderr)
            return 1
    finally:
        listener.close()
    end = time.time() + seconds + 60   # (the build ends itself at SECONDS; a minute more for a stuck one)
    tail, said = b"", False
    link.settimeout(1)
    with open(log, "wb") as out, link:
        while time.time() < end:
            try:
                data = link.recv(65536)
            except socket.timeout:
                continue
            except OSError:
                break
            if not data:
                break
            out.write(data)
            out.flush()
            if echo:
                echo(data)
            tail = (tail + data)[-4096:]
            if b"sly1: done, status" in tail:
                said = True
    return 0 if said else 1


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    sys.exit(run(sys.argv[1], sys.argv[2], float(sys.argv[3]), sys.argv[4] if len(sys.argv) > 4 else ""))


if __name__ == "__main__":
    main()
