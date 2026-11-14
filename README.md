# ESP32 RS-232
Firmware for an ESP32 that sits between a u-blox ZED-F9P GNSS receiver and an RS-232 device, made with [ESP-IDF](https://github.com/espressif/esp-idf). It passes the receiver's NMEA out of its own RS-232 port with the real number of satellites in GGA, brings NTRIP corrections to the receiver over WiFi, and shows the satellite signals in a u-center style view.

NMEA 0183 GGA stops at 12 satellites. The F9P often uses twice that, so the ESP32 replaces the GGA satellites field with the number really used in the fix, from UBX NAV-SAT (or the epoch's GSA sentences), and recalculates the checksum.

It runs on an Adafruit HUZZAH32 (ESP32 Feather) wired to a ZED-F9P and a MAX3232, see [Pinout](docs/Pinout.md).

## Features
- RS-232 NMEA output on UART2 (GPIO17): GGA with the real satellite count, plus VTG, RMC, GSA, GST and GSV, each at its own rate (off, 1, 2, 5 or 10 Hz), 4800 to 115200 baud
- Satellite signal view: one bar per satellite showing C/N0 and whether it is used in the fix, tracked, or still without an orbit, with the other band's level, like u-center
- NTRIP client, with the mountpoint picked from the caster's sourcetable, distance to the base station and correction age
- F9P receiver configuration over the UART: check and write the configuration this firmware needs, with UART1 at 460800 baud. The ESP32 finds the receiver's baud rate by itself
- Receiver fix, satellites, DOP and accuracy shown live
- WiFi Station, reachable at `http://rs232.local/` through mDNS, with an Internet health check
- WiFi Hotspot, which switches itself off once the station is connected and back on when it drops
- Firmware update from the web interface (OTA)
- CPU load, heap and uptime, log viewer, core dump download, reset to defaults

## Help
If you're setting up a new device, check out the [Getting Started](docs/Getting-Started.md) page.

To install the latest firmware check out the [Firmware Update](docs/Firmware-Update.md) page.

If you are experiencing problems with the firmware, please see the [Reporting Crashes](docs/Reporting-Crashes.md) page.

For wiring, see the [Pinout](docs/Pinout.md) page.

## Building
The firmware builds with ESP-IDF v5.4.4, the same version the GitHub Actions build uses.

```sh
git clone --recursive https://github.com/AOG-silrep/esp-rs232.git
cd esp-rs232
. $IDF_PATH/export.sh
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

If you cloned without `--recursive`, run `git submodule update --init` to fetch `components/button`.

The firmware version comes from `git describe`. After creating or moving a tag, run `idf.py reconfigure build`, as a plain `idf.py build` keeps the old version string.

## Based on
This project started from [AOG-silrep/esp32-xbee](https://github.com/AOG-silrep/esp32-xbee) at commit [`49663cb`](https://github.com/AOG-silrep/esp32-xbee/commit/49663cb), a fork of [nebkat/esp32-xbee](https://github.com/nebkat/esp32-xbee) by Nebojša Cvetković, the original firmware for the ArduSimple [WiFi NTRIP Master](https://www.ardusimple.com/product/wifi-ntrip-master/). Its history stays in that repository. The NTRIP server and caster and the TCP/UDP sockets were removed here, and the RS-232 output and the signal view added.

Licensed under the GNU General Public License v3, see [LICENSE](LICENSE).
