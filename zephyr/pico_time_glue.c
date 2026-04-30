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

/* --- Time queries ----------------------------------------------------- */

uint32_t get_time_us_32(void)
{
	/* Zephyr's k_uptime_get_32 returns ms; we need us. Use the cycle
	 * counter when available, falling back to the lower-resolution path
	 * otherwise. Pico-PIO-USB only uses this for short timeouts (≤7 us)
	 * so the resolution matters more than the absolute value. */
	uint64_t cyc = k_cycle_get_32();
	return k_cyc_to_us_floor32((uint32_t)cyc);
}

/* --- Busy waits ------------------------------------------------------- */

void busy_wait_us(uint32_t delay_us)
{
	k_busy_wait(delay_us);
}

void busy_wait_us_32(uint32_t delay_us)
{
	k_busy_wait(delay_us);
}

void busy_wait_ms(uint32_t delay_ms)
{
	k_busy_wait(delay_ms * 1000U);
}

void busy_wait_at_least_cycles(uint32_t minimum_cycles)
{
	uint32_t us = k_cyc_to_us_ceil32(minimum_cycles);
	if (us == 0) {
		us = 1;
	}
	k_busy_wait(us);
}

/* --- Alarm pool / repeating timer stubs ------------------------------- */

/* Match the upstream type layout closely enough that Pico-PIO-USB's
 * static `repeating_timer_t sof_rt` allocates the right size. None of
 * the fields are touched by the UHC driver — pio_usb_host_init's
 * start_timer() calls add_repeating_timer_us with this struct, our stub
 * here does nothing, and our 1 ms work loop drives pio_usb_host_frame()
 * instead. */

typedef struct alarm_pool alarm_pool_t;

typedef struct repeating_timer {
	int64_t delay_us;
	bool (*callback)(struct repeating_timer *rt);
	void *user_data;
	void *pool;
	void *alarm_id;
} repeating_timer_t;

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
