/*
 * Copyright (c) 2026 The Cosmos-Keyboards Contributors
 * SPDX-License-Identifier: MIT
 *
 * Shim for pico-sdk's pico/stdlib.h umbrella header. Zephyr's
 * hal_rpi_pico ships the individual hardware/x headers Pico-PIO-USB
 * needs (gpio, pio, dma, clocks, timer-structs, etc.) but not the
 * higher-level pico_stdlib library — so we satisfy the include without
 * pulling in pico_runtime / pico_stdio.
 *
 * pico_time symbols (busy_wait_x, get_time_us_32 helper, alarm_pool_x,
 * repeating_timer_x) are provided by pico_time_glue.c and declared in
 * the pico/time.h shim alongside this file.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <hardware/gpio.h>
#include <hardware/structs/timer.h>

#include "pico/time.h"
