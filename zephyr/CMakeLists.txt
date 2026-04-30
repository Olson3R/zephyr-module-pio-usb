# Copyright (c) 2026 The Cosmos-Keyboards Contributors
# SPDX-License-Identifier: MIT

if(NOT (CONFIG_UHC_PIO_USB OR CONFIG_UDC_PIO_USB))
  return()
endif()

# Pico-PIO-USB upstream sources live alongside this wrapper in
# modules/lib/pico-pio-usb (pulled in by west). It doesn't ship a
# zephyr/module.yml, so it's not visible via the standard
# ZEPHYR_<MOD>_MODULE_DIR variable — we resolve its path relative to this
# wrapper's location instead. The .pio.h files are pre-generated and
# shipped in src/ so we don't need pioasm.
set(PIO_USB_DIR ${CMAKE_CURRENT_LIST_DIR}/../../pico-pio-usb/src)
if(NOT EXISTS ${PIO_USB_DIR}/pio_usb.h)
  message(FATAL_ERROR
    "Pico-PIO-USB upstream not found at ${PIO_USB_DIR}. "
    "Is sekigon-gonnoc/Pico-PIO-USB declared at modules/lib/pico-pio-usb "
    "in your west manifest?")
endif()
zephyr_include_directories(${PIO_USB_DIR})

# Shim headers for pico-sdk umbrella headers (pico/stdlib.h, pico/bootrom.h)
# that hal_rpi_pico doesn't ship. The shim pulls in only what Pico-PIO-USB
# actually needs (the individual hardware/* headers), keeping pico_runtime /
# pico_stdio / pico_time out of the binary.
zephyr_include_directories(${CMAKE_CURRENT_LIST_DIR}/shim)

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
