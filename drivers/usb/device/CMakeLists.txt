# Copyright (c) 2026 The Cosmos-Keyboards Contributors
# SPDX-License-Identifier: MIT

zephyr_library_named(udc_pio_usb)
zephyr_library_sources(udc_pio_usb.c)
zephyr_library_link_libraries(zephyr_interface)
