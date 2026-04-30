/*
 * Copyright (c) 2026 The Cosmos-Keyboards Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Phase 2 scaffold. All callbacks return -ENOSYS — the driver compiles and
 * registers itself for any DT node with compatible "raspberrypi,pio-usb-host"
 * but does not yet drive the hardware. Real implementation lands in a
 * follow-up PR.
 */

#define DT_DRV_COMPAT raspberrypi_pio_usb_host

#include <zephyr/device.h>
#include <zephyr/drivers/usb/uhc.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(uhc_pio_usb, CONFIG_UHC_DRIVER_LOG_LEVEL);

struct uhc_pio_usb_config {
	/* Reserved for Phase 2 implementation: pinctrl, PIO phandle, data-rate. */
	uint8_t _placeholder;
};

struct uhc_pio_usb_runtime {
	/* Reserved for Phase 2 implementation: PIO state machine handles, ring
	 * buffers for in-flight transfers, etc. */
	uint8_t _placeholder;
};

static int uhc_pio_usb_lock(const struct device *dev)
{
	struct uhc_data *data = dev->data;

	return k_mutex_lock(&data->mutex, K_FOREVER);
}

static int uhc_pio_usb_unlock(const struct device *dev)
{
	struct uhc_data *data = dev->data;

	return k_mutex_unlock(&data->mutex);
}

static int uhc_pio_usb_init(const struct device *dev)
{
	ARG_UNUSED(dev);
	LOG_WRN("uhc_pio_usb is a Phase 2 scaffold — init returns -ENOSYS");
	return -ENOSYS;
}

static int uhc_pio_usb_enable(const struct device *dev)
{
	ARG_UNUSED(dev);
	return -ENOSYS;
}

static int uhc_pio_usb_disable(const struct device *dev)
{
	ARG_UNUSED(dev);
	return -ENOSYS;
}

static int uhc_pio_usb_shutdown(const struct device *dev)
{
	ARG_UNUSED(dev);
	return -ENOSYS;
}

static int uhc_pio_usb_bus_reset(const struct device *dev)
{
	ARG_UNUSED(dev);
	return -ENOSYS;
}

static int uhc_pio_usb_sof_enable(const struct device *dev)
{
	ARG_UNUSED(dev);
	return -ENOSYS;
}

static int uhc_pio_usb_bus_suspend(const struct device *dev)
{
	ARG_UNUSED(dev);
	return -ENOSYS;
}

static int uhc_pio_usb_bus_resume(const struct device *dev)
{
	ARG_UNUSED(dev);
	return -ENOSYS;
}

static int uhc_pio_usb_ep_enqueue(const struct device *dev,
				  struct uhc_transfer *const xfer)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(xfer);
	return -ENOSYS;
}

static int uhc_pio_usb_ep_dequeue(const struct device *dev,
				  struct uhc_transfer *const xfer)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(xfer);
	return -ENOSYS;
}

static const struct uhc_api uhc_pio_usb_api = {
	.lock = uhc_pio_usb_lock,
	.unlock = uhc_pio_usb_unlock,
	.init = uhc_pio_usb_init,
	.enable = uhc_pio_usb_enable,
	.disable = uhc_pio_usb_disable,
	.shutdown = uhc_pio_usb_shutdown,
	.bus_reset = uhc_pio_usb_bus_reset,
	.sof_enable = uhc_pio_usb_sof_enable,
	.bus_suspend = uhc_pio_usb_bus_suspend,
	.bus_resume = uhc_pio_usb_bus_resume,
	.ep_enqueue = uhc_pio_usb_ep_enqueue,
	.ep_dequeue = uhc_pio_usb_ep_dequeue,
};

#define UHC_PIO_USB_DEVICE_DEFINE(n)                                          \
	static const struct uhc_pio_usb_config uhc_pio_usb_cfg_##n;           \
	static struct uhc_pio_usb_runtime uhc_pio_usb_rt_##n;                 \
	static struct uhc_data uhc_pio_usb_data_##n = {                       \
		.mutex = Z_MUTEX_INITIALIZER(uhc_pio_usb_data_##n.mutex),     \
		.priv = &uhc_pio_usb_rt_##n,                                  \
	};                                                                    \
	DEVICE_DT_INST_DEFINE(n, NULL, NULL,                                  \
			      &uhc_pio_usb_data_##n,                          \
			      &uhc_pio_usb_cfg_##n,                           \
			      POST_KERNEL,                                    \
			      CONFIG_KERNEL_INIT_PRIORITY_DEVICE,             \
			      &uhc_pio_usb_api);

DT_INST_FOREACH_STATUS_OKAY(UHC_PIO_USB_DEVICE_DEFINE)
