# Copyright (c) 2026 The Cosmos-Keyboards Contributors
# SPDX-License-Identifier: MIT

# UDC build wires only the LL layer — pio_usb_device.c is deliberately
# omitted (see README.md "Architecture"). pio_usb_host.c is also omitted
# since UDC and UHC are mutually exclusive at build time.
zephyr_library_named(udc_pio_usb)

zephyr_library_sources(
  udc_pio_usb.c
  ${PIO_USB_DIR}/pio_usb.c
  ${PIO_USB_DIR}/usb_crc.c
  ${PIO_USB_GLUE_DIR}/pico_time_glue.c
)

zephyr_library_include_directories(${ZEPHYR_BASE}/drivers/usb/udc)
zephyr_library_link_libraries(zephyr_interface)
