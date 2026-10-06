"""Exercise native USB download/reset and application reconnects on a C5 station."""
import argparse
import subprocess
import sys
import time

import serial
from serial.tools import list_ports


def check_application(port, expected, timeout=10):
    deadline = time.monotonic() + timeout
    last = "no response"
    while time.monotonic() < deadline:
        connection = serial.Serial(port=None, baudrate=115200, timeout=0.3, write_timeout=1)
        # Configure before open, so a health check does not request a reset.
        connection.dtr = False
        connection.rts = False
        connection.port = port
        try:
            connection.open()
            connection.reset_input_buffer()
            connection.write(b"DEVICE_INFO\n")
            for _ in range(5):
                line = connection.readline().strip()
                if line == expected:
                    return
                if line:
                    last = repr(line)
        except serial.SerialException as error:
            last = str(error)
        finally:
            connection.close()
        time.sleep(0.2)
    raise RuntimeError(f"Application did not respond on {port}: {last}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--after", choices=("hard-reset", "watchdog-reset"), default="hard-reset",
                        help="Use ordinary USB reset for updates; full watchdog reset is diagnostic")
    parser.add_argument("--cycles", type=int, default=20,
                        help="Download/reset cycles; zero only checks application startup")
    args = parser.parse_args()
    if not 0 <= args.cycles <= 100:
        parser.error("cycles must be 0-100")
    port = next((p for p in list_ports.comports() if p.device.upper() == args.port.upper()), None)
    if not port or (port.vid, port.pid) != (0x303A, 0x1001):
        parser.error("port must be the station's native Espressif USB Serial/JTAG interface")
    mac = (port.serial_number or "").replace(":", "").lower()
    if len(mac) != 12 or any(c not in "0123456789abcdef" for c in mac):
        parser.error("USB serial number is not a 6-byte MAC")
    expected = f"DEVICE_INFO id=esp-c5-{mac}".encode()
    check_application(args.port, expected)
    print("Application identity: PASS", flush=True)
    for cycle in range(1, args.cycles + 1):
        result = subprocess.run(
            [sys.executable, "-m", "esptool", "--chip", "esp32c5", "--port", args.port,
             "--before", "usb-reset", "--after", args.after, "read-mac"],
            capture_output=True, text=True, timeout=30)
        if result.returncode:
            raise RuntimeError(f"Download cycle {cycle} failed:\n{result.stdout}{result.stderr}")
        # Independent close/open/reply checks after every automatic download/reset.
        for _ in range(3):
            check_application(args.port, expected)
        print(f"USB download/reset {cycle}/{args.cycles}, three reconnects: PASS", flush=True)
    print(f"PASS: {args.cycles} automatic download cycles; {3 * args.cycles} reconnects", flush=True)


if __name__ == "__main__":
    main()
