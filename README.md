# zephyr-module-pio-usb

Zephyr module wrapping [sekigon-gonnoc/Pico-PIO-USB](https://github.com/sekigon-gonnoc/Pico-PIO-USB) as Zephyr UHC (USB Host Controller) and UDC (USB Device Controller) drivers.

Phase 2 of the [Cosmos Lemon Wired ZMK roadmap](https://github.com/Olson3R/Cosmos-Keyboards/pull/5). Pico-PIO-USB upstream targets the RP2040 SDK + TinyUSB; this module exposes its packet-level API as standard Zephyr USB controllers so ZMK (and any other Zephyr application) can use it without TinyUSB in the binary.

## Status

**Phase 2 scaffold.** Driver shims compile but return `-ENOSYS`. The host (`uhc_pio_usb`) and device (`udc_pio_usb`) drivers, DT bindings, and standalone sample apps will land in subsequent PRs.

| Driver | Status | Wraps |
|---|---|---|
| `uhc_pio_usb` (host) | Stub | Pico-PIO-USB **host stack** (`pio_usb_host_*`) |
| `udc_pio_usb` (device) | Stub | Pico-PIO-USB **LL layer** (`pio_usb_ll_*`) directly |
| `samples/pio_usb_host/` | Not yet present | — |

## Architecture

Pico-PIO-USB has three layers (~6000 LOC total):

- **LL layer** (`pio_usb.c` + `pio_usb_ll.h` + `pio_usb.h`) — PIO programs, packet send/receive, endpoint object management, IRQ handling. ~3500 LOC of timing-critical bit-banging.
- **Host stack** (`pio_usb_host.c`) — root-port management, `pio_usb_host_init/task`, endpoint open/transfer/setup. ~800 LOC built on LL.
- **Device stack** (`pio_usb_device.c`) — owns EP0; parses standard SETUP requests and answers them from a `usb_descriptor_buffers_t` handed at `pio_usb_device_init`. ~600 LOC built on LL.

This module wraps the **host stack** for UHC because Zephyr UHC's contract ("open endpoint, queue transfer, get completion event") matches that layer 1:1.

For UDC it wraps the **LL layer directly**, *not* `pio_usb_device_*`. Zephyr's USB device stack builds descriptors dynamically and expects the controller to surface raw SETUP packets via `udc_submit_event(..., UDC_EVT_EP_REQUEST)` — the Pico-PIO-USB device stack would compete with that by answering descriptor requests itself. The UDC driver owns its own ~150 LOC of EP0 SETUP routing instead, and the wrapper's CMake omits `pio_usb_device.c` from device-mode builds.

This keeps us tracking upstream Pico-PIO-USB at the LL + host levels for the hard part (timing fixes, packet encoding) while owning a small, focused glue layer that fits Zephyr's contracts.

## How to consume

Add to your west manifest, alongside ZMK and Pico-PIO-USB upstream:

```yaml
manifest:
  remotes:
    - name: sekigon-gonnoc
      url-base: https://github.com/sekigon-gonnoc
    - name: Olson3R
      url-base: https://github.com/Olson3R
  projects:
    - name: Pico-PIO-USB
      remote: sekigon-gonnoc
      revision: 3c1eec341a5232640e4c00628b889b641af34b28  # 0.7.2
      path: modules/lib/pico-pio-usb
    - name: zephyr-module-pio-usb
      remote: Olson3R
      revision: main
      path: modules/lib/zephyr-pio-usb
```

Then enable one of the drivers per build:

```kconfig
CONFIG_UHC_PIO_USB=y   # host mode
# OR
CONFIG_UDC_PIO_USB=y   # device mode
```

A given build enables exactly one of these — Pico-PIO-USB shares PIO0 between host and device modes via different state-machine programs, so per-side ZMK builds (central vs. peripheral) pick one.

## License

MIT.
