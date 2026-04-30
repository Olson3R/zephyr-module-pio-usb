/*
 * Copyright (c) 2026 The Cosmos-Keyboards Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Pico-PIO-USB device standalone triage. Brings up udc_pio_usb on the
 * Link USB-C port and waits — no USBD-next class on top, just confirm
 * the driver init sequence reaches steady state without faulting and
 * the PIO-USB device-mode signaling holds D+ high so a host (or our
 * UHC sample on a second Lemon) sees the line in FS_IDLE.
 *
 * Logs go out the native USB-C port via legacy USBD CDC ACM. This
 * sample currently sacrifices the Link-side CDC ACM test endpoint to
 * keep the bring-up triage simple.
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/usb/udc.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(pio_usb_device_sample, LOG_LEVEL_DBG);

#define UDC_NODE DT_NODELABEL(pio_usb_device)
#define CONSOLE_NODE DT_CHOSEN(zephyr_console)

static const struct device *udc_dev = DEVICE_DT_GET(UDC_NODE);
static const struct device *console = DEVICE_DT_GET(CONSOLE_NODE);

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

static int udc_event(const struct device *dev,
		     const struct udc_event *const evt)
{
	ARG_UNUSED(dev);
	switch (evt->type) {
	case UDC_EVT_VBUS_READY:    LOG_INF("VBUS ready"); break;
	case UDC_EVT_VBUS_REMOVED:  LOG_INF("VBUS removed"); break;
	case UDC_EVT_RESET:         LOG_INF("Bus reset"); break;
	case UDC_EVT_SOF:           /* too noisy to log */ break;
	case UDC_EVT_SUSPEND:       LOG_INF("Bus suspend"); break;
	case UDC_EVT_RESUME:        LOG_INF("Bus resume"); break;
	case UDC_EVT_EP_REQUEST:    LOG_INF("EP request"); break;
	case UDC_EVT_ERROR:         LOG_WRN("Error"); break;
	default:                    LOG_DBG("UDC event %d", evt->type); break;
	}
	return 0;
}

int main(void)
{
	int ret;

	printk("\n*** UDC_MAIN_START ***\n");
	wait_for_console();
	printk("*** wait_for_console done ***\n");

	LOG_INF("Pico-PIO-USB device sample (triage) starting");

	if (!device_is_ready(udc_dev)) {
		LOG_ERR("UDC device %s not ready", udc_dev->name);
		return -ENODEV;
	}
	LOG_INF("UDC device %s present", udc_dev->name);

	ret = udc_init(udc_dev, udc_event);
	if (ret) {
		LOG_ERR("udc_init failed: %d", ret);
		return ret;
	}
	LOG_INF("udc_init OK");

	ret = udc_enable(udc_dev);
	if (ret) {
		LOG_ERR("udc_enable failed: %d", ret);
		return ret;
	}
	LOG_INF("UDC enabled — Link USB-C port should now signal as USB-FS device");

	uint32_t tick = 0;
	while (1) {
		LOG_INF("alive t=%us", tick * 2);
		tick++;
		k_msleep(2000);
	}
	return 0;
}
