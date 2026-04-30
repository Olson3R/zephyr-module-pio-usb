# zephyr-module-pio-usb

Zephyr module wrapping [sekigon-gonnoc/Pico-PIO-USB](https://github.com/sekigon-gonnoc/Pico-PIO-USB) as Zephyr UHC (USB Host Controller) and UDC (USB Device Controller) drivers.

Phase 2 of the [Cosmos Lemon Wired ZMK roadmap](https://github.com/Olson3R/Cosmos-Keyboards/pull/5). Pico-PIO-USB upstream targets the RP2040 SDK + TinyUSB; this module exposes its packet-level API as standard Zephyr USB controllers so ZMK (and any other Zephyr application) can use it without TinyUSB in the binary.

## Status

**Phase 2 scaffold.** Driver shims compile but return `-ENOSYS`. The host (`uhc_pio_usb`) and device (`udc_pio_usb`) drivers, DT bindings, and standalone sample apps will land in subsequent PRs.

| Driver | Status |
|---|---|
| `uhc_pio_usb` (host) | Stub |
| `udc_pio_usb` (device) | Stub |
| `samples/pio_usb_host/` | Not yet present |

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
