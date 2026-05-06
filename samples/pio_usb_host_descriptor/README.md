# pio_usb_host_descriptor — UHC device-descriptor dump

Standalone Pico-PIO-USB UHC sample that enumerates whatever device gets
plugged into the configured Link USB-C port (bus reset →
`GET_DESCRIPTOR(DEVICE,8)` at addr 0 → `SET_ADDRESS(1)` →
`GET_DESCRIPTOR(DEVICE,18)` → string descriptors) and prints the result
to CDC ACM on the native USB-C port. Modeled after tinyusb's
[`dual/host_info_to_device_cdc`](https://github.com/hathach/tinyusb/tree/master/examples/dual/host_info_to_device_cdc).

For the simpler "did the driver init" check (no enumeration, just
connect/disconnect events), see
[`pio_usb_host`](../pio_usb_host/README.md).

## Hardware

- A Raspberry Pi Pico (or any RP2040 board). The
  [`boards/rpi_pico.overlay`](boards/rpi_pico.overlay) puts PIO-USB on
  `GP0` (D+) / `GP1` (D-) and the CDC ACM console on the Pico's native
  USB-B port. A
  [`boards/cosmos_lemon_wired.overlay`](boards/cosmos_lemon_wired.overlay)
  is also provided for the [Cosmos Lemon
  Wired](https://ryanis.cool/cosmos/lemon/).
- A USB-FS device with a device descriptor — a flash drive, mouse,
  keyboard — wired to GP0/GP1 (typically through a USB-A or USB-C
  breakout jack).
- Native USB plugged into your computer for the CDC ACM serial console.

## Build

This repo ships a self-importing [`west.yml`](../../west.yml), so you
can build the sample directly from a clone — no other manifest needed.
First time only:

```bash
git clone https://github.com/Olson3R/zephyr-module-pio-usb.git
cd zephyr-module-pio-usb
west init -l .
west update
```

Then build (from the workspace root, i.e. the directory containing
`.west/`):

```bash
west build -b rpi_pico samples/pio_usb_host_descriptor
```

For the Cosmos Lemon Wired (board definition lives in the
[`Olson3R/rainadon-zmk`](https://github.com/Olson3R/rainadon-zmk) ZMK
fork — clone it next to this repo and pass its `app/` as
`-DBOARD_ROOT=…`):

```bash
west build -b cosmos_lemon_wired samples/pio_usb_host_descriptor \
  -- -DBOARD_ROOT=/path/to/rainadon-zmk/app
```

If you'd rather consume this module from your own application's
manifest (e.g. a ZMK fork), see [the top-level
README](../../README.md#how-to-consume) for the entries to add — once
in place, the `west build` invocation is the same.

To target a different board, supply `-b <your-board>` and place a
matching `boards/<your-board>.overlay` in this directory.

## Flash

RP2040 has no in-system programmer; flashing is BOOTSEL → drag-and-drop:

1. Hold the **BOOTSEL** button on the board and tap reset (or
   replug the native USB-C). The board enumerates as a mass-storage
   device named `RPI-RP2`.
2. Copy `build/zephyr/zephyr.uf2` onto it. The board reboots into the
   new firmware automatically.

## Expected output

Open the resulting tty (`/dev/cu.usbmodem*` on macOS,
`/dev/ttyACM0` on Linux) at any baud. After the firmware boots and the
console attaches:

```
[00:00:00.000,000] <inf> pio_usb_host_descriptor_sample: Pico-PIO-USB host descriptor-dump sample starting
[00:00:00.000,000] <inf> pio_usb_host_descriptor_sample: UHC device pio_usb_host present
[00:00:00.000,000] <inf> pio_usb_host_descriptor_sample: UHC enabled — plug a USB-FS device into the Link USB-C port
```

When you plug a device into the Link port (e.g. a Logitech keyboard):

```
[00:00:05.123,000] <inf> pio_usb_host_descriptor_sample: Device connected (full-speed)
[00:00:05.180,000] <inf> pio_usb_host_descriptor_sample: Issuing bus reset...
[00:00:05.235,000] <inf> pio_usb_host_descriptor_sample: addr=0: bMaxPacketSize0=8
[00:00:05.250,000] <inf> pio_usb_host_descriptor_sample: Device: VID=0x046d PID=0xc534
[00:00:05.250,000] <inf> pio_usb_host_descriptor_sample:   bLength             18
[00:00:05.250,000] <inf> pio_usb_host_descriptor_sample:   bDescriptorType     1
[00:00:05.250,000] <inf> pio_usb_host_descriptor_sample:   bcdUSB              0200
[00:00:05.250,000] <inf> pio_usb_host_descriptor_sample:   bDeviceClass        0
[00:00:05.250,000] <inf> pio_usb_host_descriptor_sample:   bDeviceSubClass     0
[00:00:05.250,000] <inf> pio_usb_host_descriptor_sample:   bDeviceProtocol     0
[00:00:05.250,000] <inf> pio_usb_host_descriptor_sample:   bMaxPacketSize0     8
[00:00:05.250,000] <inf> pio_usb_host_descriptor_sample:   idVendor            0x046d
[00:00:05.250,000] <inf> pio_usb_host_descriptor_sample:   idProduct           0xc534
[00:00:05.250,000] <inf> pio_usb_host_descriptor_sample:   bcdDevice           2901
[00:00:05.250,000] <inf> pio_usb_host_descriptor_sample:   iManufacturer       1  Logitech
[00:00:05.250,000] <inf> pio_usb_host_descriptor_sample:   iProduct            2  USB Receiver
[00:00:05.250,000] <inf> pio_usb_host_descriptor_sample:   iSerialNumber       0  <n/a>
[00:00:05.250,000] <inf> pio_usb_host_descriptor_sample:   bNumConfigurations  1
```

The exact VID/PID/strings depend on what's plugged in. Devices that
don't ship string descriptors will show `<n/a>` in the corresponding
field.

If `addr=0: bMaxPacketSize0=…` never appears, the
`GET_DESCRIPTOR(DEVICE,8)` round-trip is failing; check the D+/D-
wiring against the overlay's `pin-dp` / `pinout` properties and that
the device shows `Device connected` first.
