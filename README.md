# pylon2mqtt - Pylontech battery to MQTT bridge (native C)

## Overview

`pylon2mqtt` polls a Pylontech battery stack (US2000, US3000, US5000, ...
and compatibles) over its RS485 port and publishes the values to an MQTT
broker. Which values are published, and under which topics, is configurable:
you can publish a single cell voltage, all cell voltages as one JSON
message, one topic per cell, whole objects, or everything at once.

It replaces a Python script built on
[python-pylontech](https://github.com/Tomcat42/pylontech-python) (PyPI package `pylontech`) and
paho-mqtt. The protocol handling and the data tree follow python-pylontech
(`PylontechStack`). The values are identical, but `pylon2mqtt` runs as a
single small binary without a Python runtime.

The protocol is the "PYLON low voltage Protocol RS485", version 3.3.

The battery communication is also available as the shared library
**libpylontech** (`pylontech.h`), for use in your own programs. It needs
neither libmosquitto nor cJSON, only libc. See [Library](#library-libpylontech).

## Required Hardware

- A USB to RS485 adapter (e.g. FTDI FT232R based). The adapter must switch
  between transmit and receive by itself, or use RTS as transmit enable.
- Alternatively an RS485 to Ethernet/WiFi adapter in TCP server mode
  (`"port": "socket://host:port"`).

Connect the adapter to the RS485 port of the master battery. The RS485 port
uses 9600 baud by default.

## Config

Create the file `/etc/pylon2mqtt/pylon.json`. You can start from the
template file `pylon_template.json`:

```
{
  "loglevel": "INFO",
  "port": "/dev/ttyUSB0",
  "baudrate": 9600,
  "battery_count": 0,
  "interval": 2,
  "export_file_abspath": "/run/pylon2mqtt/pylon.json",
  "export_mqtt_server": "127.0.0.1",
  "export_mqtt_port": 1883,
  "export_mqtt_user": "",
  "export_mqtt_password": "",
  "export_mqtt_basetopic": "Pylon",
  "export_mqtt_status_topic": "status",
  "export_mqtt_topics": {
    "PowerW": "Calculated.Power_W",
    "RemCapAh": "Calculated.RemainCapacity_Ah",
    "RemPerc": "Calculated.Remain_Percent",
    "FullChargeReq": "ChargeDischargeManagement[0].StatusFullChargeRequired",
    "Voltage": "Analog[0].Voltage",
    "CellVoltages": "Analog[0].CellVoltages",
    "Cells/{0+1}": "Analog[0].CellVoltages[*]",
    "Calculated": "Calculated"
  }
}
```

| Key | Default | Description |
|---|---|---|
| `port` | (required) | serial device, e.g. `/dev/serial/by-id/usb-FTDI_...`, or `socket://host:port` |
| `baudrate` | `9600` | baud rate of the serial port |
| `battery_count` | `0` | number of batteries in the stack; `0` = auto detect (probes until a battery does not answer) |
| `group` | `0` | battery group, if several groups are configured |
| `interval` | `2` | seconds between the start of two polls |
| `timeout` | `1` | seconds to wait for a response from the battery |
| `loglevel` | `INFO` | `DEBUG`, `INFO`, `WARNING`, `ERROR` (`logging.INFO` is accepted too) |
| `logfile` | (stderr) | log file (rotated at 2 MiB); without it, logs go to stderr / the systemd journal |
| `export_file_abspath` | (none) | write the whole data tree as JSON to this file after every poll |
| `export_mqtt_server` | (none) | MQTT broker; without it, nothing is published |
| `export_mqtt_port` | `1883` | |
| `export_mqtt_user`, `export_mqtt_password` | (none) | empty user = anonymous |
| `export_mqtt_client_id` | `pylon2mqtt` | |
| `export_mqtt_basetopic` | `Pylon` | prefix of all topics (`""` for none) |
| `export_mqtt_topics` | see below | topic → path mapping |
| `export_mqtt_qos` | `0` | |
| `export_mqtt_retain` | `false` | publish with the retain flag |
| `export_mqtt_on_change` | `false` | only publish a topic if its payload changed (best combined with `retain`) |
| `export_mqtt_status_topic` | (none) | e.g. `status`: `<basetopic>/status` is `online` while running, `offline` otherwise (retained, last will) |
| `export_mqtt_python_format` | `false` | format payloads exactly like the Python script, see below |

Use `battery_count` if you know it: the auto detection waits for the
timeout of the first missing battery on every (re)connect.

On communication errors, `pylon2mqtt` reconnects and detects the batteries
again, like the Python version. The first error is logged as `ERROR`, the
retries only as `DEBUG`, until the communication works again.

### MQTT topics

`export_mqtt_topics` maps topic names (below `<export_mqtt_basetopic>/`)
to **paths** into the data tree. What a path points to determines the
payload:

| Path points to | Example path | Payload |
|---|---|---|
| a single value | `Analog[0].CellVoltages[2]` | the plain value: `3.166` |
| an array | `Analog[0].CellVoltages` | JSON array: `[3.15,3.135,3.166,...]` |
| an object | `Calculated` | JSON object: `{"TotalCapacity_Ah":100.0,...}` |
| the whole tree | `""` | everything as one JSON object |

Path syntax:

- `.` separates object members: `Calculated.Power_W`
- `[n]` selects array element n (0-based): `Analog[0]` is the first battery,
  `CellVoltages[0]` is the first cell
- `[*]` selects **all** array elements, `*` **all** object members; one
  message per element is published
- for every wildcard, the topic can contain a placeholder: `{0}` for the
  first wildcard, `{1}` for the second, and so on. For arrays it is replaced
  by the index, for objects by the member name. `{0+1}` adds 1 to the index
  (cells and batteries are usually numbered from 1)

Examples (with basetopic `Pylon`):

```
  "export_mqtt_topics": {
    "Cell3":               "Analog[0].CellVoltages[2]",
    "CellVoltages":        "Analog[0].CellVoltages",
    "Cells/{0+1}":         "Analog[0].CellVoltages[*]",
    "Bat{0+1}/Voltage":    "Analog[*].Voltage",
    "Bat{0+1}/Cell{1+1}":  "Analog[*].CellVoltages[*]",
    "Bat{0+1}/{1}":        "Analog[*].*",
    "Calc/{0}":            "Calculated.*",
    "Calculated":          "Calculated",
    "All":                 ""
  }
```

| Topic | Payload |
|---|---|
| `Pylon/Cell3` | `3.166` |
| `Pylon/CellVoltages` | `[3.15,3.135,3.166,...]` |
| `Pylon/Cells/1` ... `Pylon/Cells/15` | `3.15` ... `3.151` |
| `Pylon/Bat1/Voltage`, `Pylon/Bat2/Voltage`, ... | `47.323` |
| `Pylon/Bat1/Cell1` ... | one topic per cell of every battery |
| `Pylon/Bat1/Voltage`, `Pylon/Bat1/Current`, `Pylon/Bat1/CellVoltages`, ... | every value of every battery |
| `Pylon/Calc/Power_W`, `Pylon/Calc/Remain_Percent`, ... | every calculated value |
| `Pylon/Calculated` | `{"TotalCapacity_Ah":100.0,"RemainCapacity_Ah":5.64,...}` |
| `Pylon/All` | the whole data tree |

Wildcard topics adjust to the stack: with `Analog[*]` you get one topic per
battery found. If a path does not exist (e.g. `Analog[3]` with two
batteries), a warning is logged once.

Without `export_mqtt_topics`, the topics of the original Python script are
published:

| Topic | Path |
|---|---|
| `PowerW` | `Calculated.Power_W` |
| `RemCapAh` | `Calculated.RemainCapacity_Ah` |
| `RemPerc` | `Calculated.Remain_Percent` |
| `FullChargeReq` | `ChargeDischargeManagement[0].StatusFullChargeRequired` |
| `Voltage` | `Analog[0].Voltage` |
| `CellVoltages` | `Analog[0].CellVoltages` |

### Payload format

Numbers are formatted like Python (`6.0`, `47.323`, `0`). Booleans are
published as `true`/`false`, and JSON is compact
(`[3.15,3.135]`).

With `"export_mqtt_python_format": true`, payloads are byte-identical to the
Python script: booleans are `True`/`False`, and JSON uses Python's
separators (`[3.15, 3.135]`). Use this if existing consumers compare
strings.

### Data tree

This is the tree for one battery (`"export_file_abspath"` writes the same
tree to a file). With several batteries, `SerialNumbers`, `Analog`,
`ChargeDischargeManagement` and `AlarmInfo` have one entry per battery.

```
{
  "SerialNumbers": ["Y221006C50140020"],
  "Analog": [{
      "ADR": 2, "InfoFlag": 0, "CommandValue": 2,
      "CellCount": 15,
      "CellVoltages": [3.15, 3.135, 3.166, 3.153, 3.159, 3.16, 3.156, 3.148,
                       3.163, 3.146, 3.157, 3.169, 3.15, 3.16, 3.151],
      "TemperatureCount": 6,
      "Temperatures": [23.3, 22.6, 22.7, 22.4, 22.3, 22.6],
      "Current": 0.0, "Voltage": 47.323,
      "RemainCapacity": 5.64, "CapDetect": ">65Ah",
      "ModuleTotalCapacity": 100.0, "CycleNumber": 609
  }],
  "ChargeDischargeManagement": [{
      "ADR": 2, "CommandValue": 2,
      "ChargeVoltage": 53.25, "DischargeVoltage": 45.0,
      "ChargeCurrent": 80.0, "DischargeCurrent": -80.0,
      "StatusChargeEnable": true, "StatusDischargeEnable": true,
      "StatusChargeImmediately1": false, "StatusChargeImmediately2": false,
      "StatusFullChargeRequired": false
  }],
  "AlarmInfo": [{
      "ADR": 2, "InfoFlag": 0, "CommandValue": 2,
      "CellCount": 15, "CellAlarm": ["Ok", "Ok", ...],
      "TemperatureCount": 6, "TemperatureAlarm": ["Ok", ...],
      "ChargeCurrentAlarm": "Ok", "ModuleVoltageAlarm": "Ok",
      "DischargeCurrentAlarm": "Ok",
      "Status1": 0, "Status2": 14, "Status3": 0, "Status4": 0, "Status5": 0
  }],
  "SystemParameter": {
      "ADR": 2, "InfoFlag": 16,
      "CellHighVoltageLimit": 3.65, "CellLowVoltageLimit": 3.05,
      "CellUnderVoltageLimit": 2.8,
      "ChargeHighTemperatureLimit": 60.0, "ChargeLowTemperatureLimit": -10.0,
      "ChargeCurrentLimit": 100.0,
      "ModuleHighVoltageLimit": 54.0, "ModuleLowVoltageLimit": 46.0,
      "ModuleUnderVoltageLimit": 43.0,
      "DischargeHighTemperatureLimit": 60.0, "DischargeLowTemperatureLimit": -10.0,
      "DischargeCurrentLimit": -100.0
  },
  "Calculated": {
      "TotalCapacity_Ah": 100.0, "RemainCapacity_Ah": 5.64,
      "Remain_Percent": 6.0,
      "Power_W": 0.0, "ChargePower_W": 0, "DischargePower_W": -0.0,
      "CellVoltageMin": 3.135, "CellVoltageMax": 3.169, "CellVoltageDelta": 0.034
  }
}
```

Units: V, A, Ah, W and °C. `Current` and `Power_W` are positive while
charging. `Calculated` is summed over all batteries, as in
python-pylontech. `CellVoltageMin`, `CellVoltageMax` and `CellVoltageDelta`
cover all cells of all batteries.

Differences to python-pylontech's `pylonData`:

- `AnaloglList`, `ChargeDischargeManagementList` and `AlarmInfoList` are
  called `Analog`, `ChargeDischargeManagement` and `AlarmInfo`. The typo
  `ChargeCurentAlarm` is fixed to `ChargeCurrentAlarm`. The old names still
  work in paths.
- The protocol fields `VER`, `ID`, `RTN`, `LENGTH` and `PAYLOAD` are
  left out.
- `SystemParameter` is decoded according to the protocol specification.
  python-pylontech decodes it one byte off, so its values are garbage
  (e.g. `ChargeLowerLimitTemperature: -195.3`), and its field names did not
  match the values. The fields therefore have new names.
- `CellVoltageMin`/`Max`/`Delta` in `Calculated` are new.

## Migrating from pylon.py

`pylon.py` corresponds to this config:

```
{
  "port": "/dev/serial/by-id/usb-FTDI_FT232R_USB_UART_A50285BI-if00-port0",
  "baudrate": 9600,
  "battery_count": 1,
  "interval": 2,
  "export_file_abspath": "/var/log/pylon.json",
  "export_mqtt_server": "127.0.0.1",
  "export_mqtt_port": 1883,
  "export_mqtt_user": "",
  "export_mqtt_password": "",
  "export_mqtt_basetopic": "Pylon",
  "export_mqtt_python_format": true
}
```

The default topics are published with identical payloads. The export file
is JSON instead of Python's `pprint` format.

## Build

Build dependencies (Debian/Raspberry Pi OS):

`sudo apt install build-essential libmosquitto-dev libcjson-dev`

Build and install to `/usr/bin/pylon2mqtt`:

```
make pylon2mqtt
sudo make install
```

The library is compiled into `pylon2mqtt`, so the binary does not need
`libpylontech.so`. `make` without a target builds both the binary and the
library. To build only the library, which needs no extra packages, see
[Library](#library-libpylontech).

## Installation

### Debian package

Build the package (additionally needs `debhelper`):

```
cd dpkg
./build_pkg.sh
```

### Systemd automatic service

This installs and automatically starts a systemd service.

Install the debian package:

`sudo dpkg -i pylon2mqtt_...deb`

If there are problems with missing packages, run afterwards:

`sudo apt -f install`

Then create `/etc/pylon2mqtt/pylon.json` (e.g. from
`/etc/pylon2mqtt/pylon_template.json`) and restart the service.

## Start

### Automatically, if service is installed and running

The service starts automatically.

Status of the service and log:

```
sudo systemctl status pylon2mqtt
sudo journalctl -u pylon2mqtt
```

Start manually (e.g. after manually stopped)

`sudo systemctl start pylon2mqtt`

### Manually

`pylon2mqtt [config_file]` (default: `/etc/pylon2mqtt/pylon.json`)

## Stop

`sudo systemctl stop pylon2mqtt`, or CTRL+C in the foreground.

## Testing without hardware

`tests/pylon_emulator.py` emulates a battery stack with the values of a real
US3000C:

```
python3 tests/pylon_emulator.py --tcp 5000 --batteries 2 --current -12.5 &
```

Then use `"port": "socket://127.0.0.1:5000"`. The `--pty` option creates a
pseudo terminal instead and prints its device path, which tests the serial
code path.

## Library (libpylontech)

`libpylontech` contains the battery communication of `pylon2mqtt`: serial
and TCP transport, protocol, battery detection, decoding and the data tree.
It has no dependencies besides libc and libm, and does no MQTT, config file
handling or logging of its own.

### Build and install

```
make lib                 # libpylontech.so.1.0.0 (+ symlinks), needs only build-essential
sudo make install-lib    # library, pylontech.h and pylontech.pc
sudo ldconfig
```

`install-lib` installs to `/usr/lib` and `/usr/include` by default; use
`PREFIX`, `LIBDIR` and `INCLUDEDIR` to change that, e.g.
`sudo make install-lib PREFIX=/usr/local` or
`LIBDIR=/usr/lib/aarch64-linux-gnu` for Debian multiarch. `DESTDIR` is
supported for packaging.

Compile your program against it:

```
cc -o myprog myprog.c $(pkg-config --cflags --libs pylontech)
# or without installing: cc -I. -o myprog myprog.c -L. -lpylontech
```

### Usage

```c
#include <stdio.h>
#include <pylontech.h>

int main(void)
{
    pylon_config_t cfg = { .port = "/dev/ttyUSB0" };   /* or "socket://host:port" */
    pylon_t *p = pylon_new(&cfg);
    static pylon_data_t data;

    if (pylon_update(p, &data) == PYLON_OK) {
        printf("%d batteries, %.0f %%, %.1f W\n", data.battery_count,
               data.calculated.remain_percent, data.calculated.power);
        printf("cell 1: %.3f V\n", data.batteries[0].analog.cell_voltages[0]);
    } else {
        fprintf(stderr, "error: %s\n", pylon_error(p));
    }
    pylon_free(p);
    return 0;
}
```

`examples/pylon_dump.c` is a complete example that prints a summary and the
data tree as JSON (`make example`, then
`LD_LIBRARY_PATH=. examples/pylon_dump socket://127.0.0.1:5000`).

### API

All functions and types are declared and documented in
[`pylontech.h`](pylontech.h).

| Function | Description |
|---|---|
| `pylon_new(&cfg)` | create a handle; the config is copied, the port is not opened yet. `NULL` on invalid config |
| `pylon_update(p, &data)` | poll all batteries into `data`; opens the port and detects the batteries if needed |
| `pylon_connect(p)` | open the port now (optional, `pylon_update()` does it), e.g. to check the port at startup |
| `pylon_abort(p)` | make the running and all further `pylon_update()` calls return `PYLON_ERR_ABORTED`; safe from signal handlers and other threads |
| `pylon_error(p)` | text of the last error, e.g. `no response from address 2 (timeout)` |
| `pylon_strerror(rc)` | text of a return code |
| `pylon_free(p)` | close the port and free the handle |
| `pylon_data_json(&data)` | the data tree as compact JSON (free with `pylon_json_free()`) |
| `pylon_data_walk(&data, fn, ctx)` | report the data tree node by node, e.g. to build your own JSON/dict structure |
| `pylon_alarm_name(alarm)` | `Ok`, `BelowLimit`, `AboveLimit` or `OtherError` |

`pylon_config_t` (all members except `port` may be 0 for the default):

| Member | Default | Description |
|---|---|---|
| `port` | (required) | serial device or `socket://host:port` |
| `baudrate` | `9600` | |
| `battery_count` | `0` | `0` = auto detect |
| `group` | `0` | battery group 0..7 |
| `timeout` | `1` | seconds to wait for a response |
| `log`, `log_ctx` | (none) | callback `void log(void *ctx, int level, const char *msg)` for the messages `pylon2mqtt` logs (port opened, serial numbers, protocol frames at `PYLON_LOG_DEBUG`) |
| `log_level` | `0` | minimum level passed to `log` |

`pylon_update()` returns `PYLON_OK` (0) or a negative error code:
`PYLON_ERR_OPEN`, `PYLON_ERR_DETECT`, `PYLON_ERR_UPDATE` or
`PYLON_ERR_ABORTED`. After an error the port is closed, and the next call
reconnects and detects the batteries again, so you can simply call it
periodically. A call takes a few hundred milliseconds per battery (more on
timeouts); it blocks, so run it in its own thread if needed.

`pylon_data_t` holds the decoded values as plain C structs: `batteries[i]`
with `serial_number`, `analog`, `charge_management` and `alarm_info`, plus
`system_parameter` and `calculated`, the same values as in the
[data tree](#data-tree). Units are V, A, Ah, W and °C. Absent values (e.g.
`remain_percent` with a total capacity of 0) are `NAN`. The struct has
fixed size arrays (`PYLON_MAX_BATTERIES` = 16, `PYLON_MAX_CELLS` = 32,
`PYLON_MAX_TEMPERATURES` = 16) and is about 12 KiB, so don't put it on
small thread stacks.

Thread safety: a handle must not be used by several threads at the same
time, except `pylon_abort()`. Separate handles (e.g. for several stacks on
different ports) are independent.

### Use from other languages

Via the JSON functions, the library is easy to use with an FFI, e.g.
Python's `ctypes`:

```python
import ctypes, json

lib = ctypes.CDLL("libpylontech.so.1")

class Config(ctypes.Structure):
    _fields_ = [("port", ctypes.c_char_p), ("baudrate", ctypes.c_int),
                ("battery_count", ctypes.c_int), ("group", ctypes.c_int),
                ("timeout", ctypes.c_double), ("log", ctypes.c_void_p),
                ("log_ctx", ctypes.c_void_p), ("log_level", ctypes.c_int)]

lib.pylon_new.restype = ctypes.c_void_p
lib.pylon_new.argtypes = [ctypes.POINTER(Config)]
lib.pylon_update.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
lib.pylon_error.restype = ctypes.c_char_p
lib.pylon_error.argtypes = [ctypes.c_void_p]
lib.pylon_data_json.restype = ctypes.c_void_p
lib.pylon_data_json.argtypes = [ctypes.c_void_p]
lib.pylon_json_free.argtypes = [ctypes.c_void_p]
lib.pylon_free.argtypes = [ctypes.c_void_p]

cfg = Config(port=b"/dev/ttyUSB0")
p = lib.pylon_new(ctypes.byref(cfg))
data = ctypes.create_string_buffer(64 * 1024)  # opaque, >= sizeof(pylon_data_t)
if lib.pylon_update(p, data) != 0:
    raise RuntimeError(lib.pylon_error(p).decode())
s = lib.pylon_data_json(data)
tree = json.loads(ctypes.string_at(s))
lib.pylon_json_free(s)
lib.pylon_free(p)

print(tree["Calculated"]["Remain_Percent"], tree["Analog"][0]["CellVoltages"])
```

## Credits

The protocol handling (frame encoding, checksums, payload decoding) and the
data tree are ported from
[python-pylontech](https://github.com/Tomcat42/pylontech-python)
(PyPI package `pylontech`) by **Bernd Singer**, with the battery stack
abstraction (`PylontechStack`) by **Daniel Schramm**. Many thanks to them.
pylon2mqtt is a C port of their work, not an independent implementation.

## License

pylon2mqtt is licensed under the GNU General Public License, version 3 or
(at your option) any later version. See [LICENSE](LICENSE).

This includes libpylontech: programs that link against it must be
distributed under GPL-compatible terms.

python-pylontech, from which the protocol handling is ported, is licensed
under the MIT License. MIT is GPL compatible, which allows the port to be
released under the GPL. Its copyright and permission notice is kept in
[LICENSE.python-pylontech](LICENSE.python-pylontech), as the MIT License
requires.

Copyright (c) 2022 Bernd Singer (python-pylontech, MIT License)<br>
Copyright (c) 2026 Clemens Eisserer (pylon2mqtt, C port, GPL-3.0-or-later)
