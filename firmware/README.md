# Wiring and operation

| Connection | XIAO pad | GPIO |
| --- | --- | ---: |
| CO₂ SDA | D4 | 23 |
| CO₂ SCL | D5 | 24 |
| Radar TX → board RX | D7 | 12 |
| Button signals | D0, D1, D2, D3, D6, D8, D9, D10 | 1, 0, 25, 7, 11, 8, 9, 10 |

Power the SCD4x breakout from 3V3 and the LD2410C from 5V/VBUS. Connect all
grounds. The I²C pull-ups must use 3.3 V. The radar uses UART1 at 256000 baud,
8N1; its RX and OUT pins are unused. Each switch connects its signal to ground.
Use a 1 kΩ series resistor on GPIO11 because ROM UART output can occur during
reset. Application UART logging is disabled. Check breakout pin labels and fit
before connecting power; sensor carriers match the supplied CAD envelopes.

Buttons use internal pull-ups, 10 ms polling and 30 ms debounce. The event topic
reports both `press` and `release`; use `press` for immediate actions. Holding a
button does not repeat it. The separate action topic reports armed releases.
No physical button-to-function mapping is included.

The device identifies itself as `esp-c5-<MAC>` and publishes under
`esp-monitor/<device-id>/`:

| Suffix | Content |
| --- | --- |
| `availability` | Retained online/offline status |
| `state` | CO₂ ppm, temperature °C, humidity %, validity and sample time |
| `presence` | Moving/occupied flags, validity and distance |
| `buttons/event` | GPIO, pad, press/release, boot ID and sequence |
| `buttons/action` | Accepted release actions |
| `buttons/status` | Input mask, debounce/poll intervals and dropped-event counts |
| `calibration/command`, `calibration/result` | Guarded SCD4x calibration requests/results |

Air readings publish every 30 seconds; presence updates every 200 ms. Deduplicate
button events by `(boot_id, sequence)`. MQTT uses QoS 1 and button events are not
retained. The onboard LED follows fresh moving-target reports independently of
network connectivity. Wi-Fi modem power saving remains enabled.

`tools/provision.py` requires `pyserial` (`python -m pip install -r tools/requirements.txt`).
Create a broker user before MQTT setup and grant it access to its device namespace
and the Home Assistant discovery topics. Provision Wi-Fi first, then MQTT. The
broker CA is compiled into the firmware; credentials are entered locally over USB.

For an already provisioned board with this partition layout, update only the app:

```sh
idf.py build
python -m esptool --chip esp32c5 --port PORT --before usb-reset --after hard-reset write-flash --flash-mode dio --flash-size 8MB --flash-freq 80m 0x10000 build/esp_station.bin
python tests/hardware_usb.py --port PORT --cycles 0
```

The C5 rev1.0 USB-reset clock workaround is included. Never erase flash merely
to update the app; doing so removes saved credentials. OTA is not configured.
The optional UDP movement receiver is disabled by default.

Host checks (Linux/WSL with a C compiler):

```sh
python3 tests/host/test_components.py
cc -std=c11 -Wall -Wextra -Werror -Imain tests/test_radar.c main/radar.c -o /tmp/station-radar
/tmp/station-radar
cc -std=c11 -Wall -Wextra -Werror -Imain tests/test_buttons.c -o /tmp/station-buttons
/tmp/station-buttons
python3 -m unittest discover -s tests -p test_provision.py
```
