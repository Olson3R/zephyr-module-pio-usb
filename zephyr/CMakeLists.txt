# Copyright (c) 2026 The Cosmos-Keyboards Contributors
# SPDX-License-Identifier: MIT

if(NOT (CONFIG_UHC_PIO_USB OR CONFIG_UDC_PIO_USB))
  return()
endif()

# Pico-PIO-USB upstream sources are pulled in by west. It doesn't ship a
# zephyr/module.yml, so it's not visible via the standard
# ZEPHYR_<MOD>_MODULE_DIR variable. Resolve its location either:
#   - via -DPIO_USB_DIR=… on the command line (override), or
#   - by searching a list of common workspace-relative paths.
# The .pio.h files are pre-generated and shipped in src/, so we don't
# need pioasm.
if(NOT DEFINED PIO_USB_DIR)
  # Candidate locations, in priority order:
  #   1. Sibling layout (rainadon-zmk consumer): this module at
  #      modules/lib/zephyr-pio-usb, pico-pio-usb at modules/lib/pico-pio-usb.
  #   2. Standalone layout (`west init -l .` on this repo): this module is
  #      the manifest project at the workspace root, pico-pio-usb at
  #      modules/lib/pico-pio-usb relative to it.
  set(_pio_usb_candidates
    ${CMAKE_CURRENT_LIST_DIR}/../../pico-pio-usb/src
    ${CMAKE_CURRENT_LIST_DIR}/../modules/lib/pico-pio-usb/src
    ${CMAKE_CURRENT_LIST_DIR}/../../modules/lib/pico-pio-usb/src
  )
  foreach(_candidate ${_pio_usb_candidates})
    if(EXISTS ${_candidate}/pio_usb.h)
      set(PIO_USB_DIR ${_candidate})
      break()
    endif()
  endforeach()
endif()
if(NOT PIO_USB_DIR OR NOT EXISTS ${PIO_USB_DIR}/pio_usb.h)
  message(FATAL_ERROR
    "Pico-PIO-USB upstream not found. Searched: ${_pio_usb_candidates}. "
    "Either declare sekigon-gonnoc/Pico-PIO-USB at modules/lib/pico-pio-usb "
    "in your west manifest, or pass -DPIO_USB_DIR=/abs/path/to/pico-pio-usb/src.")
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

# Pico-PIO-USB marks bit-timing-sensitive functions with
# __not_in_flash_func / __time_critical_func so they execute from RAM
# rather than XIP flash — without this fragment USB packet timing breaks
# under flash-cache-miss latency. hal_rpi_pico ships an equivalent
# fragment but only enables it when CONFIG_PICOSDK_USE_FLASH=y, so we
# always pull ours in when either driver is enabled.
zephyr_linker_sources(RWDATA timecritical.ld)

if(CONFIG_UHC_PIO_USB)
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../drivers/usb/host host)
endif()

if(CONFIG_UDC_PIO_USB)
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../drivers/usb/device device)
endif()
