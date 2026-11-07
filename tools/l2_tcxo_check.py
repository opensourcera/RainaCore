#!/usr/bin/env python3
"""Report the TCXO setting Meshtastic prints at boot on a Wio Tracker L2 Pro.

This restarts the board and reads its serial log. It writes nothing to flash.
It shows the voltage the firmware requests, not an electrical measurement.
Requires esptool, which also installs pyserial.

Usage: python l2_tcxo_check.py COM19 [--seconds 30]
"""
import argparse
import re
import subprocess
import sys
import time

import serial

ANSI = re.compile(r"\x1b\[[0-9;]*m")
TCXO = re.compile(r"TCXO reference voltage(?: at ([0-9.]+) V)?")
INIT = re.compile(r"SX126x init result (-?\d+)")
VERSION = re.compile(r"\d+\.\d+\.\d+\.[0-9a-f]{7,8},[\w-]+")


def capture(port, seconds):
    """Restart the board, then return its log text."""
    subprocess.run(
        [sys.executable, "-m", "esptool", "--port", port,
         "--after", "hard-reset", "chip-id"],
        capture_output=True, timeout=40, check=True,
    )
    data = b""
    link = None
    end = time.time() + seconds
    while time.time() < end:
        if link is None:
            try:
                link = serial.Serial(port, 115200, timeout=0.2)
                link.dtr = False
                link.rts = False
            except serial.SerialException:
                time.sleep(0.05)
                continue
        try:
            data += link.read(4096)
        except serial.SerialException:
            link = None
            time.sleep(0.05)
    if link is not None:
        link.close()
    return ANSI.sub("", data.decode("latin1"))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("port", help="serial port, such as COM19 or /dev/ttyACM0")
    parser.add_argument("--seconds", type=int, default=30,
                        help="how long to read the log (default 30)")
    args = parser.parse_args()

    log = capture(args.port, args.seconds)
    version = VERSION.search(log)
    tcxo = TCXO.search(log)
    init = INIT.search(log)

    print(f"Log bytes read: {len(log)}")
    print(f"Firmware: {version.group(0) if version else 'not found in log'}")
    if tcxo is None:
        print("TCXO: not found. Debug logging may be off, or the line was missed.")
        return 1
    if tcxo.group(1) is None:
        print("TCXO: firmware reports it does not use DIO3 as the TCXO reference")
    else:
        print(f"TCXO setting requested by firmware: {float(tcxo.group(1)):.2f} V")
    print(f"Radio init result: {init.group(1) if init else 'not found in log'}")
    print("This is the firmware's setting, not a voltage measured on the board.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
