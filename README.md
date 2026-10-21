# WHB04 Network Bridge

This is ESP32-S3 firmware that puts an **XHC WHB04B wireless CNC pendant** on
your network. You plug the pendant's USB dongle into the ESP32. The
[Carvera Controller Community edition](https://github.com/Carvera-Community/Carvera_Controller),
or any other client that speaks the [bridge protocol](docs/PROTOCOL.md), then
connects to it over WiFi. That makes the pendant usable from devices that have
no free USB port, such as an Android tablet.

```
 WHB04B pendant ))) RF ((( USB dongle ──USB──▶ ESP32-S3 bridge ))) WiFi ((( Tablet / PC running
                                              (USB host)                    Carvera Controller
```

The bridge only relays the raw HID reports. All of the pendant logic (jog
wheel, buttons, macros, the LCD display) stays in the controller application.
So the pendant behaves exactly as it does when plugged into a PC by USB.

## Features

- **USB host for the WHB04B dongle** (VID:PID `10CE:EB93`), with hot-plug
  support.
- **Low-latency TCP relay** of input reports and LCD feature reports, with
  heartbeats in both directions so a dead link is noticed within about 1.5 s.
- **Setup portal**: on first boot, or when the configured WiFi can't be
  reached, the bridge opens its own access point with a page for entering
  WiFi details.
- **Optional compile-time WiFi credentials** for your own builds.
- **Web UI** at `http://<bridge>/`, showing status, WiFi settings and firmware
  updates over the air (with automatic rollback if a new image can't reach
  the network).
- **mDNS** as `<hostname>.local`, advertising the `_whb04._tcp` service.
- **Status LED** and a reset button that clears the WiFi settings.

## Hardware

Developed on a **Seeed Studio XIAO ESP32-S3**. Any ESP32-S3 board should work;
the S3 is needed for its USB OTG host support.

| What | Notes |
|------|-------|
| ESP32-S3 board | e.g. XIAO ESP32-S3 (8 MB flash) |
| USB-C OTG adapter or cable | Dongle (USB-A) → board (USB-C) |
| 5 V supply | See **Power** below |
| *(optional)* 3.3 V USB-UART adapter | For logs: XIAO **D6 = TX** (GPIO43), **D7 = RX** (GPIO44), GND |

### Power

In host mode, the dongle has to get 5 V on the USB VBUS line. Dev boards like
the XIAO have no circuit that switches VBUS on in host mode, so the dongle
only gets whatever 5 V is already present on VBUS. On the XIAO ESP32-S3, the
**5V pin is on the USB-C VBUS net**. So:

- **Feed 5 V into the 5V and GND pins.** That powers the board, and through
  the OTG adapter, the dongle.
- **Or use a powered OTG Y-adapter** that injects 5 V on the dongle side.
- **Check with a meter** that 5 V reaches the dongle end of the OTG adapter
  before relying on it.

> ⚠️ Don't connect the XIAO to a computer by USB while you're also feeding
> 5 V into the 5V pin. That would backfeed the computer's USB port. Unplug
> the external supply (or add a diode) while flashing.

### Status LED (XIAO user LED, GPIO21)

| Pattern | Meaning |
|---------|---------|
| Fast blink | Setup portal active |
| Slow blink | Connecting to WiFi |
| Short blip every 2 s | On WiFi, waiting for the dongle and/or the controller |
| Solid | Dongle attached and controller connected |

**Reset WiFi settings:** hold the **BOOT** button (GPIO0) for 5 seconds while
the bridge is running. It reboots into the setup portal. (If you use
compile-time credentials, it goes back to those.)

## Flashing

Download the firmware from the latest
[release](../../releases), or for an untagged build, from the
**whb04-bridge-firmware** artifact of a successful *Build firmware* workflow
run under **Actions**.

| File | Use |
|------|-----|
| `whb04-bridge-full.bin` | First install. Flash at offset **0x0**: it contains the bootloader, partition table and app. |
| `whb04-bridge-ota.bin`  | Updates through the bridge's web UI. |

1. **Put the board into download mode.** Hold **BOOT**, plug it into your
   computer (or press **RESET** while holding BOOT), then release BOOT.
2. **Flash the full image.** Either:
   - **From a browser:** open the [ESP web flasher](https://espressif.github.io/esptool-js/)
     in Chrome or Edge, connect, set the address to `0x0`, choose
     `whb04-bridge-full.bin`, and click *Program*.
   - **With esptool:**

     ```sh
     esptool.py --chip esp32s3 write_flash 0x0 whb04-bridge-full.bin
     ```

     Newer esptool versions spell it `esptool --chip esp32s3 write-flash 0x0 whb04-bridge-full.bin`.
3. **Press RESET** (or power-cycle) to start the firmware.

After the first install, use the web UI for updates instead.

## First-time setup

1. Power the bridge. With no WiFi configured, the LED blinks fast.
2. **Join the WiFi network `WHB04-Bridge-XXXX`** from a phone or laptop. The
   setup page should open on its own; if it doesn't, browse to
   `http://192.168.4.1/`.
3. **Enter your WiFi network and password** (use *Scan* to pick from nearby
   networks), and optionally a hostname. Then click *Save and reboot*.
4. The bridge joins your network.
5. **Give the bridge a fixed address.** A DHCP reservation on your router is
   the easiest way. Android generally can't resolve `.local` mDNS names, so
   you'll enter the IP address in the controller.

The status page is then available at `http://<bridge-ip>/`.

### Compile-time WiFi credentials

If you build the firmware yourself, you can bake in a default network. Create
`sdkconfig.defaults.local` next to `sdkconfig.defaults`. Git ignores this
file, so your credentials stay out of the repository:

```
CONFIG_BRIDGE_DEFAULT_WIFI_SSID="MyNetwork"
CONFIG_BRIDGE_DEFAULT_WIFI_PASSWORD="secret"
```

Then do a clean build (`idf.py fullclean build`). Credentials saved through the
setup portal still take precedence. Every other option is under
`idf.py menuconfig` → **WHB04 Network Bridge**, including the TCP port, the
setup-AP password, the LED and button GPIOs, VID/PID, and turning off web
updates.

## Using it with Carvera Controller

This needs a Carvera Controller build that includes the **WHB04 (Network)**
pendant type.

1. Open **Settings → Pendant**.
2. Set **Use Hardware Pendant** to **WHB04 (Network)**.
3. Set **Network Pendant Bridge Address** to the bridge's IP, for example
   `192.168.1.50`. To use a non-default port, write it as `192.168.1.50:5104`.
4. Click **Apply**. Once the bridge reports the dongle attached, the
   *Pendant Jogging* button becomes active.

If the WiFi link drops during a continuous jog, the controller stops the jog
and keeps trying to reconnect.

## Security

The pendant port (TCP 5104) and the web UI have **no authentication**. Anyone
who can reach the bridge can jog the machine, change its WiFi settings, or
upload firmware. Keep it on a trusted network or VLAN. You can also turn off
web firmware updates in `menuconfig` (`BRIDGE_WEB_OTA`).

The setup access point is open by default, but it only runs while WiFi isn't
configured or can't connect. To give it a password, set
`BRIDGE_SETUP_AP_PASSWORD` (at least 8 characters).

## Troubleshooting

- **"Pendant dongle: not detected"**: the dongle probably isn't getting 5 V
  (see **Power**). Check that its LED is lit. The serial log on D6/D7 shows
  `HID device 10CE:EB93 ...` when it's detected.
- **The controller never connects**: check the address and port, and
  confirm the status page loads from the same device. Running
  `python tools/bridge_monitor.py <bridge-ip>` from a PC prints every button
  and jog-wheel event; add `--display` to also write test values to the LCD.
- **Only one client at a time**: a second connection, such as the monitor
  tool, takes over from the controller. The controller reconnects when the
  other client disconnects.

## Building

You need [ESP-IDF v5.4](https://docs.espressif.com/projects/esp-idf/en/v5.4.2/esp32s3/get-started/)
(v5.3 or later should work). The component manager fetches dependencies
automatically.

```sh
idf.py set-target esp32s3   # only needed once
idf.py build
idf.py -p <PORT> flash       # board in download mode
```

The firmware version comes from `git describe`, so tag releases as `vX.Y.Z`.
Pushing a `v*` tag makes the GitHub workflow attach both images to a release.

## Repository layout

```
main/                 firmware sources
  usb_pendant.c       USB host + HID driver glue
  bridge_server.c     TCP server and framing
  wifi_manager.c      station + fallback setup AP
  web_ui.c            status page, WiFi setup, firmware upload
  captive_dns.c       DNS responder for the setup portal
  status_led.c        LED patterns and reset button
docs/PROTOCOL.md      wire protocol
tools/bridge_monitor.py  stand-alone test client
```

## License

GPL-2.0, the same license as the Carvera Controller. See [LICENSE](LICENSE).
