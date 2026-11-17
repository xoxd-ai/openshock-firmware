"""Reset the hub, send the serial `version` command and print the reply (TIN-7545).

Usage: python softproxy/just/read_version.py <port>

Serial runs at 115200 (platformio.ini monitor_speed). The reset pulses RTS, which the ESP32 auto-reset circuit
wires to EN, with DTR held low so GPIO0 stays high and the chip boots the app. The version reply carries no
secret. A line that matches password, token or psk is withheld anyway, and boot output is not printed.
"""

import re
import sys
import time

import serial

BAUD = 115200
BOOT_WAIT_S = 6.0  # boot log and Wi-Fi start before the serial handler reads input
REPLY_WAIT_S = 3.0
WITHHELD = re.compile(r'password|token|psk', re.IGNORECASE)


def read_for(ser: serial.Serial, seconds: float) -> bytes:
    end = time.monotonic() + seconds
    buf = bytearray()
    while time.monotonic() < end:
        chunk = ser.read(ser.in_waiting or 1)
        if chunk:
            buf.extend(chunk)
    return bytes(buf)


def main() -> int:
    if len(sys.argv) != 2:
        print('usage: read_version.py <port>', file=sys.stderr)
        return 2
    port = sys.argv[1]

    ser = serial.Serial()
    ser.port = port
    ser.baudrate = BAUD
    ser.timeout = 0.1
    ser.dtr = False
    ser.rts = False
    ser.open()
    try:
        print(f'read-version: reset via RTS on {port}')
        ser.rts = True
        time.sleep(0.2)
        ser.rts = False
        boot = read_for(ser, BOOT_WAIT_S)
        print(f'read-version: boot output {len(boot)} bytes (not printed)')
        ser.reset_input_buffer()

        ser.write(b'version\r\n')
        ser.flush()
        reply = read_for(ser, REPLY_WAIT_S)
    finally:
        ser.close()

    text = reply.decode('utf-8', errors='replace')
    lines = [line.rstrip() for line in re.split(r'\r?\n|\r', text)]
    lines = [line for line in lines if line.strip()]
    if not lines:
        print('read-version: no reply within %.1f s' % REPLY_WAIT_S, file=sys.stderr)
        return 1
    print('read-version: reply')
    for line in lines:
        if WITHHELD.search(line):
            print('  [line withheld: matches password, token or psk]')
        else:
            print('  ' + line)
    return 0


if __name__ == '__main__':
    sys.exit(main())
