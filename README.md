# zephyr-module-pio-usb

Zephyr module wrapping [sekigon-gonnoc/Pico-PIO-USB](https://github.com/sekigon-gonnoc/Pico-PIO-USB) as Zephyr UHC (USB Host Controller) and UDC (USB Device Controller) drivers.

Phase 2 of the [Cosmos Lemon Wired ZMK roadmap](https://github.com/rianadon/Cosmos-Keyboards/pull/90). Pico-PIO-USB upstream targets the RP2040 SDK + TinyUSB; this module exposes its packet-level API as standard Zephyr USB controllers so ZMK (and any other Zephyr application) can use it without TinyUSB in the binary.

## Status

**Released — `v0.1.0`.** UHC + UDC drivers, DT bindings, and standalone host/device sample apps all ship in the tag. The `uhc_pio_usb` host driver is what the ZMK USB split transport on the [Cosmos Lemon Wired](https://ryanis.cool/cosmos/lemon/) uses today (central drives the peripheral's CDC ACM bulk pair over a USB-C cable on the Link port).

| Driver | Status | Wraps |
|---|---|---|
| `uhc_pio_usb` (host) | Released, hardware-tested | Pico-PIO-USB **host stack** (`pio_usb_host_*`) |
| `udc_pio_usb` (device) | Released | Pico-PIO-USB **LL layer** (`pio_usb_ll_*`) directly |
| `samples/pio_usb_host/` | Released | Standalone UHC bring-up sample |
| `samples/pio_usb_device/` | Released | Standalone UDC bring-up sample |

## Architecture

Pico-PIO-USB has three layers (~6000 LOC total):

- **LL layer** (`pio_usb.c` + `pio_usb_ll.h` + `pio_usb.h`) — PIO programs, packet send/receive, endpoint object management, IRQ handling. ~3500 LOC of timing-critical bit-banging.
- **Host stack** (`pio_usb_host.c`) — root-port management, `pio_usb_host_init/task`, endpoint open/transfer/setup. ~800 LOC built on LL.
- **Device stack** (`pio_usb_device.c`) — owns EP0; parses standard SETUP requests and answers them from a `usb_descriptor_buffers_t` handed at `pio_usb_device_init`. ~600 LOC built on LL.

This module wraps the **host stack** for UHC because Zephyr UHC's contract ("open endpoint, queue transfer, get completion event") matches that layer 1:1. The UHC driver implements single-xfer-per-control-transfer (matching Zephyr's max3421e UHC pattern): the driver advances `xfer->stage` internally for SETUP→DATA→STATUS and only returns the xfer once STATUS completes.

For UDC it wraps the **LL layer directly**, *not* `pio_usb_device_*`. Zephyr's USB device stack builds descriptors dynamically and expects the controller to surface raw SETUP packets via `udc_submit_event(..., UDC_EVT_EP_REQUEST)` — the Pico-PIO-USB device stack would compete with that by answering descriptor requests itself. The UDC driver owns its own ~150 LOC of EP0 SETUP routing instead, and the wrapper's CMake omits `pio_usb_device.c` from device-mode builds.

This keeps us tracking upstream Pico-PIO-USB at the LL + host levels for the hard part (timing fixes, packet encoding) while owning a small, focused glue layer that fits Zephyr's contracts.

## How to consume

Add to your west manifest:

```yaml
manifest:
  remotes:
    - name: sekigon-gonnoc
      url-base: https://github.com/sekigon-gonnoc
    - name: Olson3R
      url-base: https://github.com/Olson3R
  projects:
    # Pico-PIO-USB upstream. Post-0.7.2 main includes commit 38ed543
    # ("optimize to reduce delay between received DATA and sending
    # handshake") which inlines pio_usb_bus_send_handshake and caches
    # PIO/SM/rx_buffer in locals. Without this fix, the device misses
    # our ACK on the SET_ADDRESS IN STATUS at full-speed (must arrive
    # within 6.5–7.5 bit-times of EOP per USB §7.1.18) and never
    # commits the new address.
    - name: Pico-PIO-USB
      remote: sekigon-gonnoc
      revision: 675543bcc9baa8170f868ab7ba316d418dbcf41f
      path: modules/lib/pico-pio-usb
    - name: zephyr-module-pio-usb
      remote: Olson3R
      revision: v0.1.0
      path: modules/lib/zephyr-pio-usb
```

> If your application imports a ZMK fork's `app/west.yml` (e.g. `Olson3R/rainadon-zmk`), it already pulls Pico-PIO-USB and this wrapper transitively — you don't need to list them again in your own manifest.

Then enable one of the drivers per build:

```kconfig
CONFIG_UHC_PIO_USB=y   # host mode
# OR
CONFIG_UDC_PIO_USB=y   # device mode
```

A given build enables exactly one of these — Pico-PIO-USB shares PIO0 between host and device modes via different state-machine programs, so per-side ZMK builds (central vs. peripheral) pick one.

## License

MIT.
