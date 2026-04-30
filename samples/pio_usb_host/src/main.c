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
#include <zephyr/drivers/usb/uhc.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/usb/usb_ch9.h>

LOG_MODULE_REGISTER(pio_usb_host_sample, LOG_LEVEL_DBG);

#define UHC_NODE DT_NODELABEL(pio_usb_host)

static const struct device *uhc_dev = DEVICE_DT_GET(UHC_NODE);

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

	LOG_INF("Pico-PIO-USB host sample starting");

	if (!device_is_ready(uhc_dev)) {
		LOG_ERR("UHC device %s not ready", uhc_dev->name);
		return -ENODEV;
	}

	ret = uhc_init(uhc_dev, uhc_event);
	if (ret) {
		LOG_ERR("uhc_init failed: %d", ret);
		return ret;
	}

	ret = uhc_enable(uhc_dev);
	if (ret) {
		LOG_ERR("uhc_enable failed: %d", ret);
		return ret;
	}

	LOG_INF("UHC enabled — plug a USB-FS device into the Link USB-C port");

	/* Periodically probe: if a device just connected, perform a bus
	 * reset (required before issuing any transfers, per USB 2.0
	 * §9.1.2) and log progress. We don't issue an explicit
	 * GET_DESCRIPTOR(DEVICE) here because that needs a net_buf pool
	 * + endpoint allocation that the USB host stack normally manages
	 * — for this sample, surfacing the connect/reset path is enough
	 * to confirm the controller is alive. */
	bool reset_done = false;
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
		k_msleep(500);
	}

	return 0;
}
