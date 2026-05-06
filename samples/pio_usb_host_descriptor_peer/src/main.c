/*
 * SPDX-License-Identifier: MIT
 *
 * Test peer for samples/pio_usb_host_descriptor. The legacy USB device
 * stack auto-initializes at boot (CONFIG_USB_DEVICE_INITIALIZE_AT_BOOT)
 * with the VID/PID/strings pinned in prj.conf; main itself has nothing
 * to do beyond keeping the kernel running so the USB device thread
 * stays scheduled.
 */

#include <zephyr/kernel.h>

int main(void)
{
	while (1) {
		k_sleep(K_FOREVER);
	}
	return 0;
}
