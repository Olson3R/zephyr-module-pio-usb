# pio_usb_device — UDC bring-up triage

Standalone Pico-PIO-USB UDC sample. Brings up `udc_pio_usb` on the
configured DT node and waits — no USBD-next class on top, just confirms
the driver init sequence reaches steady state without faulting and
that PIO-USB device-mode signaling holds D+ high so a host (or the
[`pio_usb_host`](../pio_usb_host/README.md) sample on a second board)
sees the line in FS_IDLE.

This sample sacrifices the Link-side test endpoint to keep the
bring-up triage simple — logs go out the **native** USB-C port via the
legacy USBD CDC ACM. For a UHC test on the same board family, see
[`pio_usb_host`](../pio_usb_host/README.md).

## Hardware

- An RP2040 board with two USB-C ports (or any board with a
  `raspberrypi,pio-usb-device` DT node). The default board overlay
  targets the [Cosmos Lemon
  Wired](https://ryanis.cool/cosmos/lemon/), which puts D+/D- on
  `GP0`/`GP1`.
- Native USB-C plugged into your computer for the CDC ACM serial
  console.
- (Optional, end-to-end test) A second board running
  [`pio_usb_host`](../pio_usb_host/README.md), with a USB-C cable
  between the two boards' Link ports.

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
west build -b cosmos_lemon_wired samples/pio_usb_device
```

If you'd rather consume this module from your own application's
manifest (e.g. a ZMK fork), see [the top-level
README](../../README.md#how-to-consume) for the entries to add — once
in place, the `west build` invocation above is the same.

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
[00:00:00.000,000] <inf> pio_usb_device_sample: Pico-PIO-USB device sample (triage) starting
[00:00:00.000,000] <inf> pio_usb_device_sample: UDC device pio_usb_device present
[00:00:00.000,000] <inf> pio_usb_device_sample: udc_init OK
[00:00:00.000,000] <inf> pio_usb_device_sample: UDC enabled — Link USB-C port should now signal as USB-FS device
[00:00:02.000,000] <inf> pio_usb_device_sample: alive t=2s
```

Heartbeat every 2 s confirms the UDC driver thread is running. No
class is registered on the Link port, so a host plugged into Link will
see the line as a USB-FS device that doesn't enumerate fully — that's
expected.

## End-to-end test with `pio_usb_host`

Pair this sample with [`pio_usb_host`](../pio_usb_host/README.md) on a
second board for a Lemon-to-Lemon link smoke test:

1. **Board A:** flash this sample, native USB-C into computer A.
2. **Board B:** flash `pio_usb_host`, native USB-C into computer B.
3. Cable Board A's Link USB-C ↔ Board B's Link USB-C.
4. On Board B's serial console you should see
   `Device connected (full-speed)` followed by `Bus reset complete`.
   On Board A's console you should see the heartbeat continue
   uninterrupted (no fault on bus reset).
