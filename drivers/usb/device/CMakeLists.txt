# Copyright (c) 2026 The Cosmos-Keyboards Contributors
# SPDX-License-Identifier: MIT

# UDC build wires the LL layer plus pio_usb_device.c — but the device
# stack's standard-request handlers in pio_usb_device.c are unreachable
# at runtime because we override the WEAK pio_usb_device_irq_handler
# with our own implementation that routes raw SETUP packets into Zephyr
# UDC events instead. See README.md "Architecture" and the file header
# comment in udc_pio_usb.c for the full reasoning.
#
# pio_usb_host.c is omitted since UDC and UHC are mutually exclusive at
# build time (one PIO instance, one program).
zephyr_library_named(udc_pio_usb)

zephyr_library_sources(
  udc_pio_usb.c
  ${PIO_USB_DIR}/pio_usb.c
  ${PIO_USB_DIR}/pio_usb_device.c
  ${PIO_USB_DIR}/usb_crc.c
  ${PIO_USB_GLUE_DIR}/pico_time_glue.c
  ${PIO_USB_GLUE_DIR}/pico_irq_glue.c
)

zephyr_library_include_directories(${ZEPHYR_BASE}/drivers/usb/udc)
zephyr_library_link_libraries(zephyr_interface)
