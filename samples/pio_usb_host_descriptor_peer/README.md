# pio_usb_host_descriptor_peer — descriptor-dump test peer

Deterministic USB-FS device companion to
[`pio_usb_host_descriptor`](../pio_usb_host_descriptor/README.md). Flash
this on a second RP2040 board, plug its native USB-C port into the host
board's Link USB-C port, and the host's descriptor dump should match
the **Expected output** block in
`pio_usb_host_descriptor/README.md` exactly.

The peer presents a single CDC ACM interface on the device's native
USB block with a fixed VID/PID/manufacturer/product/serial set:

| Field             | Value                                |
| ----------------- | ------------------------------------ |
| `idVendor`        | `0x2fe3` (Zephyr Project)            |
| `idProduct`       | `0x42aa`                             |
| `bcdDevice`       | `0x0100`                             |
| `bMaxPacketSize0` | `64`                                 |
| iManufacturer     | `zephyr-pio-usb`                     |
| iProduct          | `PIO-USB descriptor host test peer`  |
| iSerialNumber     | `DESCTEST`                           |

No serial console, no application logic — just the legacy USB device
stack initialized at boot. `main()` sleeps forever; the USB thread
handles all enumeration traffic.

## Hardware

- A second RP2040 board (Raspberry Pi Pico, Cosmos Lemon Wired, etc.).
  Native USB-B/USB-C is what the host enumerates; Link / PIO pins are
  unused on the peer.
- The peer's native USB connected to the host's Link USB-C through a
  USB cable. **Plug into the *native* USB-C, not the Link USB-C** — on
  the Lemon Wired the Link port has no native USB function.

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
west build -b rpi_pico samples/pio_usb_host_descriptor_peer
```

For the Cosmos Lemon Wired (the board definition lives in `rianadon/zmk:main`
under `app/boards/arm/cosmos_lemon_wired`; clone that fork and pass its
`app/` as `BOARD_ROOT`):

```bash
west build -b cosmos_lemon_wired samples/pio_usb_host_descriptor_peer \
  -- -DBOARD_ROOT=/path/to/zmk-fork/app
```

## Flash

BOOTSEL → drag-and-drop, the standard RP2040 flow:

1. Hold **BOOTSEL** on the peer board and tap reset (or replug its
   native USB-C into your computer). The board mounts as `RPI-RP2`.
2. Copy `build/zephyr/zephyr.uf2` onto it. The board reboots into the
   peer firmware automatically.

## Verify

The peer has no serial output of its own. The way to know it's working
is to plug the peer's native USB-C into the host's Link USB-C and read
the host's CDC ACM console — you should see the `Expected output`
block from `pio_usb_host_descriptor/README.md` with VID `0x2fe3` /
PID `0x42aa` / iProduct `PIO-USB descriptor host test peer`.

If the host stays in `idle` (no `Device connected` event):

- Confirm the cable bridges VBUS from host's Link USB-C to peer's
  native USB-C — without VBUS the peer's RP2040 won't boot, and its USB
  function never engages.
- Confirm you flashed the peer's native USB-C, not its Link port.
- Confirm the peer's power LED is on when the cable is connected.

If the host fires `Device connected (full-speed)` but enumeration
still fails, the bug is in the *host*, not this peer — check that the
host is built from a `pio_usb_host_descriptor` that includes commit
`baeafa6` (`samples: pio_usb_host_descriptor: open EP0 with MPS=64,
not 8`); pre-`baeafa6` the descriptor sample silently retries forever
when the peer responds with a 64-byte first DATA0.
