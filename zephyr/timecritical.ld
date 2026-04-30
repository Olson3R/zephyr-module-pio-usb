/* Copyright (c) 2026 The Cosmos-Keyboards Contributors
 * SPDX-License-Identifier: MIT
 *
 * Pico-PIO-USB marks its bit-timing-sensitive functions with
 * __not_in_flash_func / __time_critical_func so they execute from RAM
 * rather than XIP flash. Without this, USB packet timing breaks under
 * flash-cache-miss latency on RP2040.
 *
 * Pulled in via zephyr_linker_sources(RWDATA ...) so the section is
 * loaded into RAM at boot. Same shape as hal_rpi_pico's existing
 * timecritical.ld (which only kicks in when CONFIG_PICOSDK_USE_FLASH=y).
 */
*(.time_critical*)
