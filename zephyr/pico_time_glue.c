/*
 * Copyright (c) 2026 The Cosmos-Keyboards Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Pico-PIO-USB references several pico_time symbols that Zephyr's
 * hal_rpi_pico doesn't ship (pico_time isn't in the Kconfig-selectable
 * subset). We provide Zephyr-backed implementations here so the upstream
 * sources link cleanly inside a Zephyr build.
 *
 * The repeating-timer family is reduced to no-ops: the UHC driver runs
 * its own 1 ms work loop that calls pio_usb_host_frame() directly, so
 * Pico-PIO-USB's internal alarm-pool timer never needs to fire. The
 * driver also passes skip_alarm_pool=true in pio_usb_configuration_t to
 * make sure pio_usb_host_init never tries to create a real alarm pool.
 *
 * Backed by k_busy_wait / k_uptime_get_32 / k_cyc_to_us_floor32 — these
 * are the Zephyr equivalents of the pico_time primitives Pico-PIO-USB
 * uses for sub-millisecond timing.
 */

#include <zephyr/kernel.h>
#include <stdbool.h>
#include <stdint.h>

#include "pico/time.h"

/* Note: pico-sdk's get_time_us_32() function is shadowed inside
 * Pico-PIO-USB by a macro in usb_crc.h that reads timer_hw->timerawl
 * directly (the RP2040 system timer). That register is exposed via
 * hardware/structs/timer.h from hal_rpi_pico. We therefore don't need
 * to provide get_time_us_32() ourselves. */

/* --- Busy waits ------------------------------------------------------- */
/* busy_wait_us / _us_32 / _ms come from hal_rpi_pico's hardware_timer
 * (provided when CONFIG_PICOSDK_USE_TIMER=y). Only busy_wait_at_least_cycles
 * is missing — that one isn't in pico-sdk's hardware_timer either,
 * Pico-PIO-USB references it from src/pio_usb.c. */

void busy_wait_at_least_cycles(uint32_t minimum_cycles)
{
	uint32_t us = k_cyc_to_us_ceil32(minimum_cycles);
	if (us == 0) {
		us = 1;
	}
	k_busy_wait(us);
}

/* --- Alarm pool / repeating timer stubs ------------------------------- */
/* Pico-PIO-USB's static `repeating_timer_t sof_rt` allocates with the
 * struct layout declared in pico/time.h. The driver thread drives
 * pio_usb_host_frame() itself on a 1 ms work loop, so the alarm-pool
 * timer never needs to fire — the stubs here just satisfy linkage. */

alarm_pool_t *alarm_pool_create(uint32_t hardware_alarm_num, uint32_t max_timers)
{
	(void)hardware_alarm_num;
	(void)max_timers;
	/* We never use the pool — the driver thread / work loop drives
	 * pio_usb_host_frame() directly. Returning NULL is intentional;
	 * Pico-PIO-USB's start_timer() falls through to the
	 * default-pool variant (also a no-op below). */
	return NULL;
}

bool alarm_pool_add_repeating_timer_us(alarm_pool_t *pool,
				       int64_t delay_us,
				       bool (*callback)(repeating_timer_t *),
				       void *user_data,
				       repeating_timer_t *out)
{
	(void)pool;
	if (out != NULL) {
		out->delay_us = delay_us;
		out->callback = callback;
		out->user_data = user_data;
	}
	return true;
}

bool add_repeating_timer_us(int64_t delay_us,
			    bool (*callback)(repeating_timer_t *),
			    void *user_data,
			    repeating_timer_t *out)
{
	if (out != NULL) {
		out->delay_us = delay_us;
		out->callback = callback;
		out->user_data = user_data;
	}
	return true;
}

bool cancel_repeating_timer(repeating_timer_t *timer)
{
	(void)timer;
	return true;
}
