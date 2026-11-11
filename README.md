# RockBLOCK-960x

[![CI](https://github.com/rock7/RockBLOCK-960x/actions/workflows/ci.yml/badge.svg)](https://github.com/rock7/RockBLOCK-960x/actions/workflows/ci.yml)
[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

Official C and Python library for the RockBLOCK 9602, 9603 and 9604 Iridium SBD modems, maintained by [Ground Control](https://www.groundcontrol.com).

- Runs on Linux, macOS, Windows, Arduino and PlatformIO
- Python bindings built from the same C code
- Hardware documentation: [RockBLOCK 9604](https://docs.groundcontrol.com/iot/rockblock-9604), [RockBLOCK 9602/9603](https://docs.groundcontrol.com/iot/rockblock)

The C API can be seen in the generated Doxygen documentation [here](https://rock7.github.io/RockBLOCK-960x/) or the `rockblock960x.h` header file.

## Quick snippets

These snippets assume the modem is already open. They use 115200 baud for a 9604; use 19200 for a 9602/9603.

**C**

```c
rb960x_t rb;
rb960xBegin(&rb, "/dev/ttyUSB0", 115200, NULL, NULL);
```

**Python**

```python
import rockblock960x

rb = rockblock960x.RockBlock960x()
rb.begin("/dev/ttyUSB0", baud=115200)
```

### Check the RAT mode

9604 only. A 9602/9603 only does SBD, so it reports unknown.

**C**

```c
if (rb.ratMode == RB960X_RAT_SBD)
{
    printf("SBD\n");
}
else if (rb.ratMode == RB960X_RAT_LTE_CAT_M1)
{
    printf("LTE Cat M1\n");
}
```

**Python**

```python
print(rb.rat_mode.name)  # SBD, LTE_CAT_M1 or UNKNOWN
```

### Switch the RAT mode

9604 only. Takes a few seconds while the modem resets.

**C**

```c
if (rb960xSwitchRat(&rb, RB960X_RAT_LTE_CAT_M1) == RB960X_OK)
{
    printf("Now in LTE Cat M1\n");
}
```

**Python**

```python
rb.switch_rat(rockblock960x.RatMode.LTE_CAT_M1)
```

Use `RB960X_RAT_SBD` / `RatMode.SBD` to switch back for SBD messages.

### Check for signal

**C**

```c
uint8_t bars;
if (rb960xGetSignal(&rb, true, &bars) == RB960X_OK)
{
    printf("Signal: %u/5\n", bars);
}
```

**Python**

```python
print(f"Signal: {rb.get_signal(fresh=True)}/5")
```

### Check the mailbox

A session with an empty MO buffer. Like any session, it can cost credits.

**C**

```c
rb960xSessionResult_t result;
rb960xClearBuffers(&rb, RB960X_BUFFER_MO);
if (rb960xInitiateSession(&rb, false, RB960X_SBDIX_TIMEOUT_MS, &result) == RB960X_OK)
{
    if (result.mtStatus == 1)
    {
        printf("MT message received\n");
    }
    printf("Still queued: %u\n", result.mtQueued);
}
```

**Python**

```python
rb.clear_buffers()
result = rb.initiate_session()
if result.mt_status == 1:
    print("MT message received")
print(f"Still queued: {result.mt_queued}")
```

### Read an MT message

Reads the message the last session received.

**C**

```c
char msg[RB960X_MT_MAX];
size_t len;
if (rb960xReadMtBuffer(&rb, msg, sizeof(msg), &len) == RB960X_OK)
{
    printf("MT: %.*s\n", (int)len, msg);
}
```

**Python**

```python
msg = rb.read_mt_buffer()  # None if there isn't one
```

### Send an MO message

**C**

```c
const char msg[] = "Hello";
rb960xSessionResult_t result;
rb960xWriteMoBuffer(&rb, msg, strlen(msg));
rb960xStatus_t status = rb960xInitiateSession(&rb, false, RB960X_SBDIX_TIMEOUT_MS, &result);
if (status == RB960X_OK)
{
    printf("Sent\n");
}
else if (status == RB960X_ERR_SESSION)
{
    printf("Failed: %s\n", rb960xMoStatusText(result.moResult));
}
```

**Python**

```python
rb.write_mo_buffer("Hello")
result = rb.initiate_session()
if result.sent:
    print("Sent")
else:
    print(f"Failed: {result.mo_status_text}")
```

### Send an AT command the library doesn't have

**C**

```c
char resp[128];
if (atClientSendCmd(&rb.at, "AT+CGMI", resp, sizeof(resp), 5000) == AT_OK)
{
    printf("%s\n", resp);
}
```

**Python**

```python
print(rb.command("AT+CGMI"))
```

### View debug output

Pass a handler when opening the modem to print every line sent (TX) and received (RX). The examples do this with `-D`.

**C**

```c
static void debugHandler(atDebugDir_t dir, const char *data, size_t len, void *userData)
{
    (void)userData;
    if (dir == AT_DEBUG_TX)
    {
        printf("TX > %.*s\n", (int)len, data);
    }
    else
    {
        printf("RX < %.*s\n", (int)len, data);
    }
}

rb960xBegin(&rb, "/dev/ttyUSB0", 115200, debugHandler, NULL);
```

**Python**

```python
def debug(direction, data):
    print(direction, data)

rb.begin("/dev/ttyUSB0", baud=115200, debug=debug)
```

## Things to know

- **Sky view:** the antenna needs a clear view of the sky. Indoors, under cover or near trees and buildings, sessions will often fail.
- **Every session can cost credits:** this includes a session that only checks the mailbox. The library never starts one by itself.
- **Every session also checks for MT messages:** a message it receives is left in the modem's MT buffer. Read it (or clear it) before the next session, which would otherwise erase it. Until then, sessions return `RB960X_ERR_MT_UNREAD`.
- **Blocking:** most calls return quickly, but `rb960xInitiateSession()` waits for the session to finish, which can take a minute or more. To keep your program running meanwhile, use `rb960xInitiateSessionAsync()` and call `rb960xProcess()` regularly (`initiate_session_async()` and `process()` in Python). See `examples/sdb_send_async.c` and `python/examples/send_receive_async.py`.
- **Retrying is up to you:** a session makes one attempt. If it fails, `rb960xMoStatusText()` (`result.mo_status_text` in Python) says why. `rb960xSessionReady()` (`session_ready()`) is a quick, free check that a session is worth attempting now.
- **Settings:** the limits and timeouts in `src/rockblock960x_config.h` can be changed without editing the file, e.g. `-DRB960X_SBD_SESSION_TIMEOUT_S=120`, or `build_flags` in `platformio.ini`.

## Examples

All take `-d <serial device>`, `-b <baud>` (default 115200; use 19200 for a 9602/9603) and `-D` for debug output.

| C (in `build/`) | What it does |
| --- | --- |
| `rb960x_device_info` | Prints the model, IMEI and firmware versions |
| `rb960x_sdb_send -m "Hello"` | Sends one message |
| `rb960x_sdb_send_async -m "Hello"` | Sends one message without blocking |
| `rb960x_sdb_receive` | Listens for ring alerts and reads MT messages |

`rb960x_send_receive` is the simplest example: no options, with the port set at the top of `examples/send_receive.c`.

| Python (in `python/examples/`) | What it does |
| --- | --- |
| `rockblock.py -d /dev/ttyUSB0 info` | Prints the modem's details. Also `signal`, `send`, `at` and `rat` |
| `send_receive_async.py -d /dev/ttyUSB0 -m "Hello"` | Sends one message without blocking |
| `send_receive.py` | The simplest example, with the port set in the file |

## Python

Python 3.10 or later, CMake and a C compiler (on Windows, Visual Studio as below). From the repository root:

```sh
pip install ./python
```

Or straight from GitHub:

```sh
pip install "git+https://github.com/rock7/RockBLOCK-960x#subdirectory=python"
```

## Building from source on Linux and macOS

Needs CMake and a C compiler. From the repository root:

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

The examples are in `build/`, e.g.:

```sh
./build/rb960x_device_info -d /dev/ttyUSB0
```

On macOS, use the `/dev/cu.usbserial-*` device, not `/dev/tty.usbserial-*`.

## Building from source on Windows

### Dependencies

- [Silicon Labs CP2105 Universal Windows Driver](https://www.silabs.com/interface/usb-bridges/classic/device.cp2105?tab=softwareandtools), only needed when connecting a RockBLOCK 9604 through its USB-C port
- [CMake](https://cmake.org/download/)
- [Visual Studio](https://visualstudio.microsoft.com/) with the **Desktop development with C++** workload, plus **Python development** to build the Python bindings
- Optional: [Python](https://www.python.org/downloads/) (64-bit)

### Build and test

Open the **Developer Command Prompt for VS** (from the Start menu), which puts `msbuild` on the path, then from the repository root:

```bat
cmake -A x64 -S . -B build
msbuild build\ALL_BUILD.vcxproj /p:Configuration=Debug /p:Platform=x64
ctest --test-dir build -C Debug --output-on-failure
```

Choose the options to suit:

- **Platform:** `x64` (most PCs), `Win32` (32-bit) or `ARM64`. Use the same value for `-A` and `/p:Platform`. To use the Python bindings, it must match your Python (e.g. 64-bit Python needs `x64`).
- **Configuration:** `Debug` or `Release`. Use the same value for `/p:Configuration` and `ctest -C`.

The examples are in `build\<Configuration>`, e.g.:

```bat
build\Debug\rb960x_device_info.exe -d COM4
```

Use the COM port shown in Device Manager, and add `-b 19200` for a 9602/9603.

## Arduino and PlatformIO

Install the library, call `begin()` on the serial port, then pass the port to `rb960xBegin()`:

```cpp
#include <rockblock960x.h>

static rb960x_t rb;

void setup()
{
    Serial1.begin(115200); // 19200 for a 9602/9603
    if (rb960xBegin(&rb, Serial1) == RB960X_OK)
    {
        // Ready
    }
}
```

Any `Stream` works, including `SoftwareSerial`. See `examples/Basic/Basic.ino`.

## Using it in your own CMake project

Add this repository to yours (e.g. as a git submodule), then:

```cmake
cmake_minimum_required(VERSION 3.14)
project(my_project C)

add_subdirectory(RockBLOCK-960x)

add_executable(my_app main.c)
target_link_libraries(my_app PRIVATE rockblock960x)
```

The library's examples, tests and Python library aren't built this way.

## Documentation

The Doxygen HTML in `docs/` is generated from the comments in `src/` and committed, so GitHub Pages can serve it. After changing the public API, regenerate it (needs [Doxygen](https://www.doxygen.nl/) and [Graphviz](https://graphviz.org/)) and commit the result:

```sh
cmake -S . -B build
cmake --build build --target docs
```

## Licence

MIT, see [LICENSE](LICENSE). Maintained by [Ground Control](https://www.groundcontrol.com).
