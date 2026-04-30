# Copyright (c) 2026 The Cosmos-Keyboards Contributors
# SPDX-License-Identifier: MIT

if(CONFIG_UHC_PIO_USB OR CONFIG_UDC_PIO_USB)
  zephyr_include_directories(${ZEPHYR_PICO_PIO_USB_MODULE_DIR}/src)
endif()

if(CONFIG_UHC_PIO_USB)
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../drivers/usb/host host)
endif()

if(CONFIG_UDC_PIO_USB)
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../drivers/usb/device device)
endif()
