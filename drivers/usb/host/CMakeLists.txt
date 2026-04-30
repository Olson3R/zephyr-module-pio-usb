# Copyright (c) 2026 The Cosmos-Keyboards Contributors
# SPDX-License-Identifier: MIT

zephyr_library_named(uhc_pio_usb)

zephyr_library_sources(
  uhc_pio_usb.c
  ${PIO_USB_DIR}/pio_usb.c
  ${PIO_USB_DIR}/pio_usb_host.c
  ${PIO_USB_DIR}/usb_crc.c
  ${PIO_USB_GLUE_DIR}/pico_time_glue.c
)

# uhc_common.h is a Zephyr-internal driver helper. Reach into the Zephyr
# tree the same way the in-tree drivers/usb/uhc/uhc_max3421e.c does.
zephyr_library_include_directories(${ZEPHYR_BASE}/drivers/usb/uhc)
zephyr_library_link_libraries(zephyr_interface)
