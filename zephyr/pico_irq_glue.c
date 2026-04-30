/*
 * Copyright (c) 2026 The Cosmos-Keyboards Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Zephyr-backed shims for pico-sdk's hardware_irq runtime helpers.
 * hal_rpi_pico exposes the hardware/irq.h header but doesn't compile
 * irq.c — pulling that file in would fight Zephyr's NVIC management
 * via direct VTOR manipulation. Instead we adapt the two pico-sdk APIs
 * Pico-PIO-USB actually calls (`pio_usb_device_init` for the PIO RX IRQ
 * registration) onto Zephyr's irq_connect_dynamic / irq_enable.
 */

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/irq.h>

/* pico-sdk's irq_handler_t is a parameterless function pointer. */
typedef void (*irq_handler_t)(void);

void irq_set_exclusive_handler(unsigned num, irq_handler_t handler)
{
	/* Zephyr's IRQ_CONNECT macro is build-time only. For runtime
	 * registration we use irq_connect_dynamic, which requires
	 * CONFIG_DYNAMIC_INTERRUPTS=y in the application's prj.conf. The
	 * `void *` parameter expected by the dynamic ISR is unused —
	 * Pico-PIO-USB's IRQ handlers take no argument. */
	irq_connect_dynamic(num, 0,
			    (void (*)(const void *))handler, NULL, 0);
}

void irq_set_enabled(unsigned num, bool enabled)
{
	if (enabled) {
		irq_enable(num);
	} else {
		irq_disable(num);
	}
}
