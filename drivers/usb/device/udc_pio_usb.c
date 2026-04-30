/*
 * Copyright (c) 2026 The Cosmos-Keyboards Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Phase 2 scaffold. All callbacks return -ENOSYS — the driver compiles and
 * registers itself for any DT node with compatible "raspberrypi,pio-usb-device"
 * but does not yet drive the hardware. Real implementation lands in a
 * follow-up PR.
 *
 * Architecture (decided 2026-04-29; see README.md "Architecture"):
 *
 * The real driver wraps the Pico-PIO-USB **LL layer** (pio_usb_ll_*)
 * directly, NOT pio_usb_device_*. Pico-PIO-USB's device stack owns EP0
 * and answers standard descriptor requests internally from the
 * usb_descriptor_buffers_t handed at pio_usb_device_init — but Zephyr's
 * USB device stack builds descriptors dynamically and expects the
 * controller to surface raw SETUP packets via
 *   udc_submit_event(..., UDC_EVT_EP_REQUEST).
 * The two enumeration state machines would fight, so we skip
 * pio_usb_device.c entirely (CMake omits it from device-mode builds) and
 * own ~150 LOC of EP0 SETUP routing here.
 */

#define DT_DRV_COMPAT raspberrypi_pio_usb_device

#include <zephyr/device.h>
#include <zephyr/drivers/usb/udc.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(udc_pio_usb, CONFIG_UDC_DRIVER_LOG_LEVEL);

struct udc_pio_usb_config {
	uint8_t _placeholder;
};

struct udc_pio_usb_runtime {
	uint8_t _placeholder;
};

static int udc_pio_usb_lock(const struct device *dev)
{
	struct udc_data *data = dev->data;

	return k_mutex_lock(&data->mutex, K_FOREVER);
}

static int udc_pio_usb_unlock(const struct device *dev)
{
	struct udc_data *data = dev->data;

	return k_mutex_unlock(&data->mutex);
}

static int udc_pio_usb_init(const struct device *dev)
{
	ARG_UNUSED(dev);
	LOG_WRN("udc_pio_usb is a Phase 2 scaffold — init returns -ENOSYS");
	return -ENOSYS;
}

static int udc_pio_usb_enable(const struct device *dev)
{
	ARG_UNUSED(dev);
	return -ENOSYS;
}

static int udc_pio_usb_disable(const struct device *dev)
{
	ARG_UNUSED(dev);
	return -ENOSYS;
}

static int udc_pio_usb_shutdown(const struct device *dev)
{
	ARG_UNUSED(dev);
	return -ENOSYS;
}

static int udc_pio_usb_set_address(const struct device *dev, const uint8_t addr)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(addr);
	return -ENOSYS;
}

static int udc_pio_usb_host_wakeup(const struct device *dev)
{
	ARG_UNUSED(dev);
	return -ENOSYS;
}

static enum udc_bus_speed udc_pio_usb_device_speed(const struct device *dev)
{
	ARG_UNUSED(dev);
	return UDC_BUS_SPEED_FS;
}

static int udc_pio_usb_ep_enable(const struct device *dev,
				 struct udc_ep_config *const cfg)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cfg);
	return -ENOSYS;
}

static int udc_pio_usb_ep_disable(const struct device *dev,
				  struct udc_ep_config *const cfg)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cfg);
	return -ENOSYS;
}

static int udc_pio_usb_ep_set_halt(const struct device *dev,
				   struct udc_ep_config *const cfg)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cfg);
	return -ENOSYS;
}

static int udc_pio_usb_ep_clear_halt(const struct device *dev,
				     struct udc_ep_config *const cfg)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cfg);
	return -ENOSYS;
}

static int udc_pio_usb_ep_enqueue(const struct device *dev,
				  struct udc_ep_config *const cfg,
				  struct net_buf *const buf)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cfg);
	ARG_UNUSED(buf);
	return -ENOSYS;
}

static int udc_pio_usb_ep_dequeue(const struct device *dev,
				  struct udc_ep_config *const cfg)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cfg);
	return -ENOSYS;
}

static const struct udc_api udc_pio_usb_api = {
	.lock = udc_pio_usb_lock,
	.unlock = udc_pio_usb_unlock,
	.init = udc_pio_usb_init,
	.enable = udc_pio_usb_enable,
	.disable = udc_pio_usb_disable,
	.shutdown = udc_pio_usb_shutdown,
	.set_address = udc_pio_usb_set_address,
	.host_wakeup = udc_pio_usb_host_wakeup,
	.device_speed = udc_pio_usb_device_speed,
	.ep_enable = udc_pio_usb_ep_enable,
	.ep_disable = udc_pio_usb_ep_disable,
	.ep_set_halt = udc_pio_usb_ep_set_halt,
	.ep_clear_halt = udc_pio_usb_ep_clear_halt,
	.ep_enqueue = udc_pio_usb_ep_enqueue,
	.ep_dequeue = udc_pio_usb_ep_dequeue,
};

#define UDC_PIO_USB_DEVICE_DEFINE(n)                                          \
	static const struct udc_pio_usb_config udc_pio_usb_cfg_##n;           \
	static struct udc_pio_usb_runtime udc_pio_usb_rt_##n;                 \
	static struct udc_data udc_pio_usb_data_##n = {                       \
		.mutex = Z_MUTEX_INITIALIZER(udc_pio_usb_data_##n.mutex),     \
		.priv = &udc_pio_usb_rt_##n,                                  \
	};                                                                    \
	DEVICE_DT_INST_DEFINE(n, NULL, NULL,                                  \
			      &udc_pio_usb_data_##n,                          \
			      &udc_pio_usb_cfg_##n,                           \
			      POST_KERNEL,                                    \
			      CONFIG_KERNEL_INIT_PRIORITY_DEVICE,             \
			      &udc_pio_usb_api);

DT_INST_FOREACH_STATUS_OKAY(UDC_PIO_USB_DEVICE_DEFINE)
