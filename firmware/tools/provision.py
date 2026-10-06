"""Provision Wi-Fi or MQTT over native USB without storing host-side passwords."""
import argparse
from getpass import getpass
import time


def checked(value, maximum, minimum=1):
    if any(c in value for c in "\r\n\0") or not minimum <= len(value.encode()) <= maximum:
        raise ValueError(f"Input must be {minimum}-{maximum} UTF-8 bytes without line breaks")
    return value


def exchange(connection, kind, fields, timeout=90):
    deadline = time.monotonic() + timeout
    connection.write((kind + "_SETUP\n").encode())
    index = 0
    while time.monotonic() < deadline:
        line = connection.readline().decode('utf-8', errors='replace').strip()
        if line.startswith((kind + "_ERROR", kind + "_FAILED")):
            raise RuntimeError("Device rejected setup; check connection and credentials")
        if index < len(fields) and line == fields[index][0]:
            connection.write((fields[index][1] + "\n").encode())
            index += 1
        elif line.startswith(kind + "_CONNECTED"):
            if index != len(fields):
                raise RuntimeError("Unexpected setup response")
            return
    raise TimeoutError("Setup timed out; reconnect USB before trying again")


def main():
    import serial
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True)
    parser.add_argument('kind', choices=['wifi', 'mqtt'])
    args = parser.parse_args()
    if args.kind == 'wifi':
        ssid = checked(input('Wi-Fi SSID: '), 32)
        password = checked(getpass('Wi-Fi password (blank for open network): '), 63, 0)
        if password and len(password.encode()) < 8:
            raise ValueError('Wi-Fi password must have at least 8 bytes')
        fields = [('WIFI_SSID_REQUEST', ssid), ('WIFI_PASSWORD_REQUEST', password)]
    else:
        host = checked(input('MQTT broker hostname: '), 127)
        port = int(input('MQTT TLS port [8883]: ') or '8883')
        if not 1 <= port <= 65535:
            raise ValueError('Invalid TCP port')
        user = checked(input('MQTT username: '), 63)
        password = checked(getpass('MQTT password: '), 95)
        fields = [('MQTT_HOST_REQUEST', host), ('MQTT_PORT_REQUEST', str(port)),
                  ('MQTT_USERNAME_REQUEST', user), ('MQTT_PASSWORD_REQUEST', password)]
    connection = serial.Serial(port=None, baudrate=115200, timeout=0.25, write_timeout=3)
    connection.dtr = False
    connection.rts = False
    connection.port = args.port
    try:
        connection.open()
        connection.reset_input_buffer()
        exchange(connection, args.kind.upper(), fields)
        print('Connected; settings saved on the station.')
    finally:
        connection.close()
        fields.clear()
        password = None


if __name__ == '__main__':
    main()
