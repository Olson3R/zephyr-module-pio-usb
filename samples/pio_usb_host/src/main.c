/*
 * Copyright (c) 2026 The Cosmos-Keyboards Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Pico-PIO-USB host standalone sample. Brings up uhc_pio_usb on a
 * Lemon Wired (or any RP2040 board with a raspberrypi,pio-usb-host DT
 * node) and logs UHC events for any device plugged into the configured
 * Link USB-C port.
 *
 * Logs go out the native USB-C port via CDC ACM. Open the resulting
 * tty (/dev/cu.usbmodem* on macOS, /dev/ttyACM0 on Linux) at any baud.
 *
 * Pair with samples/pio_usb_device/ for an end-to-end Lemon-to-Lemon
 * test:
 *
 *   Lemon A: flash this sample, native USB into computer A
 *   Lemon B: flash pio_usb_device sample, native USB into computer B
 *   Cable Lemon A's Link USB-C → Lemon B's Link USB-C
 *
 * Both Lemons should log a connect event and a successful descriptor
 * fetch.
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/usb/uhc.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/usb/usb_ch9.h>

LOG_MODULE_REGISTER(pio_usb_host_sample, LOG_LEVEL_DBG);

#define UHC_NODE DT_NODELABEL(pio_usb_host)
#define CONSOLE_NODE DT_CHOSEN(zephyr_console)

static const struct device *uhc_dev = DEVICE_DT_GET(UHC_NODE);
static const struct device *console = DEVICE_DT_GET(CONSOLE_NODE);

/* Wait until the host opens the CDC ACM port (asserts DTR) so the boot
 * logs aren't dropped into a buffer that nobody's reading yet. Time out
 * after ~3s so the sample still does work even if no console is
 * attached. */
static void wait_for_console(void)
{
	uint32_t dtr = 0;
	for (int i = 0; i < 30; i++) {
		printk("wfc i=%d dtr=%u\n", i, dtr);
		uart_line_ctrl_get(console, UART_LINE_CTRL_DTR, &dtr);
		if (dtr) {
			break;
		}
		k_msleep(100);
	}
	k_msleep(200);
}

/* Global flag: a device is connected and we should send a descriptor
 * fetch on the next loop iteration. The UHC event callback runs in the
 * driver thread, so we don't issue transfers from there — main does it. */
static atomic_t connected;
static uint8_t connected_speed;

static int uhc_event(const struct device *dev, const struct uhc_event *const evt)
{
	ARG_UNUSED(dev);

	switch (evt->type) {
	case UHC_EVT_DEV_CONNECTED_LS:
	case UHC_EVT_DEV_CONNECTED_FS:
	case UHC_EVT_DEV_CONNECTED_HS:
		LOG_INF("Device connected (%s-speed)",
			evt->type == UHC_EVT_DEV_CONNECTED_LS ? "low" :
			evt->type == UHC_EVT_DEV_CONNECTED_HS ? "high" :
			"full");
		connected_speed = evt->type;
		atomic_set(&connected, 1);
		break;
	case UHC_EVT_DEV_REMOVED:
		LOG_INF("Device removed");
		atomic_clear(&connected);
		break;
	case UHC_EVT_RESETED:
		LOG_INF("Bus reset complete");
		break;
	case UHC_EVT_SUSPENDED:
		LOG_INF("Bus suspended");
		break;
	case UHC_EVT_RESUMED:
		LOG_INF("Bus resumed");
		break;
	case UHC_EVT_RWUP:
		LOG_INF("Remote wakeup");
		break;
	case UHC_EVT_EP_REQUEST:
		LOG_INF("EP request complete: ep=0x%02x err=%d len=%u",
			evt->xfer ? evt->xfer->ep : 0xff,
			evt->xfer ? evt->xfer->err : -1,
			evt->xfer && evt->xfer->buf ? evt->xfer->buf->len : 0);
		break;
	case UHC_EVT_ERROR:
		LOG_WRN("UHC error status=%d", evt->status);
		break;
	default:
		LOG_DBG("UHC event %d", evt->type);
		break;
	}
	return 0;
}

int main(void)
{
	int ret;

	printk("\n*** MAIN_START ***\n");
	wait_for_console();
	printk("*** wait_for_console done ***\n");

	LOG_INF("Pico-PIO-USB host sample starting");

	if (!device_is_ready(uhc_dev)) {
		LOG_ERR("UHC device %s not ready", uhc_dev->name);
		return -ENODEV;
	}
	LOG_INF("UHC device %s present", uhc_dev->name);

	ret = uhc_init(uhc_dev, uhc_event);
	if (ret) {
		LOG_ERR("uhc_init failed: %d", ret);
		return ret;
	}
	LOG_INF("uhc_init OK");

	ret = uhc_enable(uhc_dev);
	if (ret) {
		LOG_ERR("uhc_enable failed: %d", ret);
		return ret;
	}

	LOG_INF("UHC enabled — plug a USB-FS device into the Link USB-C port");

	/* Periodically probe: if a device just connected, perform a bus
	 * reset (required before issuing any transfers, per USB 2.0
	 * §9.1.2) and log progress. Always emit a heartbeat every 2s so
	 * the host can confirm the firmware is alive even when no device
	 * is connected. */
	bool reset_done = false;
	uint32_t tick = 0;
	while (1) {
		if (atomic_get(&connected) && !reset_done) {
			LOG_INF("Issuing bus reset...");
			ret = uhc_bus_reset(uhc_dev);
			if (ret) {
				LOG_ERR("uhc_bus_reset failed: %d", ret);
			} else {
				reset_done = true;
			}
		} else if (!atomic_get(&connected)) {
			reset_done = false;
		}
		if ((tick % 4) == 0) {
			LOG_INF("alive t=%us link=%s", tick / 2,
				atomic_get(&connected) ? "connected" : "idle");
		}
		tick++;
		k_msleep(500);
	}

	return 0;
}
