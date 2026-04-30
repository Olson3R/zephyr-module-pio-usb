/*
 * Copyright (c) 2026 The Cosmos-Keyboards Contributors
 * SPDX-License-Identifier: MIT
 *
 * Shim for pico-sdk's pico/time.h. Zephyr's hal_rpi_pico doesn't
 * package pico_time, so we declare here exactly the type+function
 * surface Pico-PIO-USB references; implementations live in
 * pico_time_glue.c.
 *
 * The repeating-timer family is no-op-backed because the UHC/UDC
 * drivers run their own 1 ms work loops; Pico-PIO-USB's internal SOF
 * timer never fires (the drivers also pass skip_alarm_pool=true at
 * init).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Busy waits. busy_wait_us/_us_32/_ms come from hal_rpi_pico (pico-sdk
 * hardware_timer, enabled via CONFIG_PICOSDK_USE_TIMER). Only the cycle
 * variant is provided by our glue. */
void busy_wait_us(uint64_t delay_us);
void busy_wait_us_32(uint32_t delay_us);
void busy_wait_ms(uint32_t delay_ms);
void busy_wait_at_least_cycles(uint32_t minimum_cycles);

/* Note: get_time_us_32 is a macro defined in usb_crc.h that reads
 * timer_hw->timerawl directly (the RP2040 system timer's lower 32 bits).
 * That works in our build because hardware/structs/timer.h is included
 * via the stdlib.h shim above. */

/* Alarm pool / repeating timer — no-op stubs (see pico_time_glue.c). */
typedef struct alarm_pool alarm_pool_t;

typedef struct repeating_timer {
	int64_t delay_us;
	bool (*callback)(struct repeating_timer *rt);
	void *user_data;
	void *pool;
	void *alarm_id;
} repeating_timer_t;

alarm_pool_t *alarm_pool_create(uint32_t hardware_alarm_num,
				uint32_t max_timers);

bool alarm_pool_add_repeating_timer_us(alarm_pool_t *pool,
				       int64_t delay_us,
				       bool (*callback)(repeating_timer_t *),
				       void *user_data,
				       repeating_timer_t *out);

bool add_repeating_timer_us(int64_t delay_us,
			    bool (*callback)(repeating_timer_t *),
			    void *user_data,
			    repeating_timer_t *out);

bool cancel_repeating_timer(repeating_timer_t *timer);

#ifdef __cplusplus
}
#endif
