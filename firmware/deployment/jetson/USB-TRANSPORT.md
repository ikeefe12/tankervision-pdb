# Deployment USB transport

The deployment application uses the ESP32-S3 USB-OTG controller with TinyUSB CDC on J2. The earlier board test used USB Serial/JTAG (`HWCDC`), whose host-open reset behavior was observed on this board. Switching controllers removes that transport's reset mechanism; the deployment transport still needs a bench check across open, close, unplug, and power transitions.

## Arduino ESP32 3.3.3 configuration

Use these board options:

```text
esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=default,FlashSize=8M,PartitionScheme=default_8MB,PSRAM=opi,FlashMode=qio
```

`USBMode=default` means USB-OTG/TinyUSB (`ARDUINO_USB_MODE=0`). `CDCOnBoot=default` disables automatic CDC startup. Keep MSC and DFU on boot disabled. An explicit CDC object lets the application disable reboot hooks before making USB visible:

```cpp
#include <USB.h>
#include <USBCDC.h>

USBCDC JetsonUsb;

void startJetsonUsb() {
  JetsonUsb.enableReboot(false);
  JetsonUsb.setRxBufferSize(2048);
  JetsonUsb.setTxTimeoutMs(2);
  JetsonUsb.begin(115200);
  USB.begin();
}
```

Use the explicit object for every protocol byte. With CDC on boot disabled, `Serial` is not this USB endpoint. Do not route debug output to this endpoint, wait indefinitely for a host, or enable USB DFU.

The exact installed 3.3.3 core was inspected, including [`boards.txt`](https://github.com/espressif/arduino-esp32/blob/3.3.3/boards.txt), [`main.cpp`](https://github.com/espressif/arduino-esp32/blob/3.3.3/cores/esp32/main.cpp), and [`USBCDC.cpp`](https://github.com/espressif/arduino-esp32/blob/3.3.3/cores/esp32/USBCDC.cpp). In this release:

- The constructor defaults reboot support to enabled. `enableReboot(false)` disables both its DTR/RTS bootloader sequence and its 1200-baud bootloader trigger.
- CDC on boot starts USB in `app_main()`, before application `setup()`. Disabling reboot in `setup()` while leaving automatic startup enabled creates an avoidable startup window.
- `operator bool()` reports connected only after **both DTR and RTS are asserted**. Writes also depend on TinyUSB reporting a connected CDC interface.
- A zero transmit timeout makes the write deadline already expired. Use a small nonzero timeout, handle short writes, and preserve line framing across retries. USB work must not block the power supervisor.

The [official CDC API](https://docs.espressif.com/projects/arduino-esp32/en/latest/api/usb_cdc.html) documents explicit CDC startup and the reboot/timeout APIs. The 3.3.3 [TinyUSB HAL implementation](https://github.com/espressif/arduino-esp32/blob/3.3.3/cores/esp32/esp32-hal-tinyusb.c) initializes the internal PHY for the OTG controller. This change requires neither eFuse programming nor an external PHY.

## Host behavior

The deployment client opens at 115200 baud, asserts DTR and RTS before opening, and leaves them stable. This is deliberate for **deployment TinyUSB CDC with reboot disabled**. It is not a safe substitute for the previous HWCDC test logger's opening policy. Do not run the deployment client against older board-test firmware while operating on supercap power.

The client never pulses control lines, requests 1200 baud, invokes a bootloader tool, or resets the device to recover communication. Firmware must continue power management when the host is absent or not reading. A Linux `/dev/serial/by-id/` path is preferable to a changing `/dev/ttyACM*` number. Linux opens use pyserial's exclusive mode. Device disappearance ends the example client; a service may restart it using the same persistent device identity.

Disabling the CDC reboot hooks also removes automatic serial-triggered upload entry. Plan manual BOOT/RESET recovery with main power present when flashing subsequent firmware. Do not reset a board that currently depends on its software-controlled backup rail.

## Acceptance check

With main power present, record `boot_id` and increasing `uptime_ms`, then open/close/reopen the client and unplug/replug J2. The same boot identifier and monotonic uptime must continue. Repeat host operations during a separately authorized backup test while observing the supervisor deadline. Confirm telemetry resumes and that no USB operation itself releases backup power. A successful software compile does not establish these hardware results.
