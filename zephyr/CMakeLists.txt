# Copyright (c) 2026 The Cosmos-Keyboards Contributors
# SPDX-License-Identifier: MIT

if(NOT (CONFIG_UHC_PIO_USB OR CONFIG_UDC_PIO_USB))
  return()
endif()

# Pico-PIO-USB upstream sources live in modules/lib/pico-pio-usb (pulled in
# by west). The .pio.h files are pre-generated and shipped in src/ so we
# don't need pioasm.
set(PIO_USB_DIR ${ZEPHYR_PICO_PIO_USB_MODULE_DIR}/src)
zephyr_include_directories(${PIO_USB_DIR})

# Each driver compiles the relevant Pico-PIO-USB sources into its own
# zephyr_library inside the subdirectory below — pio_usb_host.c for UHC,
# nothing extra for UDC (LL only). Both drivers also pull in the
# pico_time_glue.c shim that backs the pico-sdk timing functions Zephyr's
# hal_rpi_pico doesn't ship.
set(PIO_USB_GLUE_DIR ${CMAKE_CURRENT_LIST_DIR})

if(CONFIG_UHC_PIO_USB)
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../drivers/usb/host host)
endif()

if(CONFIG_UDC_PIO_USB)
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../drivers/usb/device device)
endif()
