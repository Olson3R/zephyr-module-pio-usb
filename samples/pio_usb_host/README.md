# pio_usb_host — UHC bring-up triage

Standalone Pico-PIO-USB UHC sample. Brings up `uhc_pio_usb` on the
configured DT node, registers a UHC event callback, issues exactly one
bus reset on first connect, and heartbeats every 2 s. Intended as the
minimum signal that the driver init sequence reaches steady state and
that connect/disconnect events are flowing — does not enumerate the
attached device. For descriptor enumeration, see
[`pio_usb_host_descriptor`](../pio_usb_host_descriptor/README.md).

## Hardware

- A Raspberry Pi Pico (or any RP2040 board). The
  [`boards/rpi_pico.overlay`](boards/rpi_pico.overlay) puts PIO-USB on
  `GP0` (D+) / `GP1` (D-) and the CDC ACM console on the Pico's native
  USB-B port. A
  [`boards/cosmos_lemon_wired.overlay`](boards/cosmos_lemon_wired.overlay)
  is also provided for the [Cosmos Lemon
  Wired](https://ryanis.cool/cosmos/lemon/), which uses the same pinout
  but routes PIO-USB through the Link USB-C port.
- A USB-FS device wired to GP0/GP1 — typically through a USB-A or
  USB-C breakout jack so you can plug in a flash drive, mouse, or
  keyboard.
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
west build -b rpi_pico samples/pio_usb_host
```

For the Cosmos Lemon Wired (board definition lives in the
[`Olson3R/rainadon-zmk`](https://github.com/Olson3R/rainadon-zmk) ZMK
fork — clone it next to this repo and pass its `app/` as
`-DBOARD_ROOT=…`):

```bash
west build -b cosmos_lemon_wired samples/pio_usb_host \
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
console attaches, you should see:

```
[00:00:00.000,000] <inf> pio_usb_host_sample: Pico-PIO-USB host sample starting
[00:00:00.000,000] <inf> pio_usb_host_sample: UHC device pio_usb_host present
[00:00:00.000,000] <inf> pio_usb_host_sample: uhc_init OK
[00:00:00.000,000] <inf> pio_usb_host_sample: UHC enabled — plug a USB-FS device into the Link USB-C port
[00:00:02.000,000] <inf> pio_usb_host_sample: alive t=2s link=idle
```

When you plug a device into the Link port:

```
[00:00:05.123,000] <inf> pio_usb_host_sample: Device connected (full-speed)
[00:00:05.500,000] <inf> pio_usb_host_sample: Issuing bus reset...
[00:00:05.550,000] <inf> pio_usb_host_sample: Bus reset complete
[00:00:06.000,000] <inf> pio_usb_host_sample: alive t=6s link=connected
```

Unplugging:

```
[00:00:10.000,000] <inf> pio_usb_host_sample: Device removed
```

If you see `UHC device pio_usb_host present` but no connect events
when a device is attached, double-check the D+/D- wiring against the
overlay's `pin-dp` / `pinout` properties.
