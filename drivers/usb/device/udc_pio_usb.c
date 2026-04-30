/*
 * Copyright (c) 2026 The Cosmos-Keyboards Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Zephyr UDC (USB Device Controller) driver wrapping sekigon-gonnoc/Pico-PIO-USB
 * in device mode.
 *
 * Architecture (see README.md "Architecture" for the full reasoning):
 *
 *   Pico-PIO-USB ships its own EP0 standard-request stack inside
 *   pio_usb_device.c — pio_usb_device_init takes a usb_descriptor_buffers_t
 *   and the built-in IRQ handler (__pio_usb_device_irq_handler) calls
 *   process_device_setup_stage() to answer GET_DESCRIPTOR / SET_ADDRESS /
 *   SET_CONFIGURATION / etc. directly out of those buffers.
 *
 *   That's incompatible with Zephyr's USB device stack, which builds
 *   descriptors dynamically and expects the controller to surface raw
 *   SETUP packets via udc_ctrl_submit_*.
 *
 *   We bypass Pico-PIO-USB's stack by overriding the WEAK alias
 *   pio_usb_device_irq_handler with our own implementation. The base
 *   PIO RX IRQ handler (usb_device_packet_handler in pio_usb_device.c)
 *   still runs — it does the actual byte-level packet receive,
 *   ACK/NAK, and address matching. Our handler picks up where the
 *   built-in stack leaves off:
 *
 *     - PIO_USB_INTS_RESET_END_BITS  → re-init endpoint pool, surface UDC_EVT_RESET
 *     - PIO_USB_INTS_SETUP_REQ_BITS  → read root->setup_packet, build a
 *                                      net_buf via udc_ctrl_alloc + submit
 *                                      via udc_ctrl_submit_s_*_status
 *     - PIO_USB_INTS_ENDPOINT_COMPLETE_BITS → drain ep_complete bitmap,
 *                                      forward queued buffers to the
 *                                      device stack via udc_submit_ep_event
 *
 *   Pico-PIO-USB's `usb_descriptor_buffers_t` parameter to
 *   pio_usb_device_init is passed as a zero-filled struct — the built-in
 *   request handlers that would consult it are unreachable through our
 *   override, and the global `descriptor_buffers` static remains unused.
 *
 * Threading: a driver thread takes the work-pending semaphore, polls for
 * the IRQ-set flags, and dispatches them. Queued IN buffers are kicked off
 * via pio_usb_ll_transfer_start. The 1 ms cadence isn't critical for device
 * mode (the IRQ handles host-driven traffic) — the thread mainly bridges
 * IRQ-context flags to thread-context UDC submissions which expect to
 * allocate net_bufs and call into upper layers.
 *
 * Status: first-iteration code, builds clean but NOT validated on hardware.
 */

#define DT_DRV_COMPAT raspberrypi_pio_usb_device

#include <zephyr/device.h>
#include <zephyr/drivers/usb/udc.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/usb/usb_ch9.h>

/* Zephyr-internal driver helper header — pulled via the include path
 * added in zephyr/CMakeLists.txt → drivers/usb/device/CMakeLists.txt. */
#include "udc_common.h"

/* Pico-PIO-USB headers. pio_usb_ll.h declares the LL packet ops + the
 * device endpoint helpers; pio_usb_device.c provides the packet-level
 * IRQ handler that we don't override. */
#include "pio_usb.h"
#include "pio_usb_configuration.h"
#include "pio_usb_ll.h"
#include "usb_definitions.h"

LOG_MODULE_REGISTER(udc_pio_usb, CONFIG_UDC_DRIVER_LOG_LEVEL);

/* Eight bidirectional endpoints (EP0..7). PIO_USB_DEV_EP_CNT in upstream
 * is 16 endpoint *numbers*, but the EP pool layout is 2*num + dir, so 16
 * total slots cover EP0–7 in/out — plenty for the split-USB use case. */
#define UDC_PIO_USB_NUM_EPS 8

/* Notification flags drained by the driver thread. Set from our IRQ
 * override, cleared after dispatch. */
#define WORK_FLAG_RESET     BIT(0)
#define WORK_FLAG_SETUP     BIT(1)
#define WORK_FLAG_EP_DONE   BIT(2)
#define WORK_FLAG_CONNECT   BIT(3)
#define WORK_FLAG_DISCONNECT BIT(4)

struct udc_pio_usb_cfg {
	struct udc_ep_config *ep_cfg_in;
	struct udc_ep_config *ep_cfg_out;
	uint8_t pin_dp;
	bool pinout_dpdm;
};

struct udc_pio_usb_data_priv {
	struct k_thread thread;
	struct k_sem work_sem;
	atomic_t flags;
	bool initialized;
	bool enabled;
	bool stop;
	uint8_t pending_address;     /* set_address — applied on STATUS-IN */
	bool address_pending;
};

K_KERNEL_STACK_DEFINE(udc_pio_usb_thread_stack,
		      CONFIG_UDC_PIO_USB_THREAD_STACK_SIZE);

/* Pico-PIO-USB has one device instance globally; we mirror that. */
static const struct device *udc_pio_usb_singleton;

static inline struct udc_pio_usb_data_priv *priv_for(const struct device *dev)
{
	return udc_get_private(dev);
}

/* ===== EP pool slot mapping ==========================================
 *
 * Pico-PIO-USB's pio_usb_device_get_endpoint_by_address() uses
 *   slot = (ep_num << 1) | dir_bit
 * where dir_bit is 1 for IN (since 0x80 >> 7 = 1). EP0 OUT is slot 0,
 * EP0 IN is slot 1, EP1 OUT is slot 2, EP1 IN is slot 3, etc. This must
 * stay in sync with upstream — pio_usb_device_init initialises slots 0/1
 * as the EP0 control pair.
 */

/* Forward an IN-direction transfer for an endpoint by handing the upper
 * net_buf's payload directly to pio_usb_ll_transfer_start. Returns 0 on
 * accept, negative errno on configuration error. */
static int kick_off_in(uint8_t ep, uint8_t *buf, uint16_t len)
{
	endpoint_t *pep = pio_usb_device_get_endpoint_by_address(ep);
	if (pep->size == 0) {
		LOG_ERR("ep 0x%02x not configured", ep);
		return -EINVAL;
	}
	if (!pio_usb_ll_transfer_start(pep, buf, len)) {
		LOG_ERR("ll_transfer_start(0x%02x) refused", ep);
		return -EIO;
	}
	return 0;
}

/* Set up an OUT-direction receive buffer the same way. */
static int kick_off_out(uint8_t ep, uint8_t *buf, uint16_t len)
{
	endpoint_t *pep = pio_usb_device_get_endpoint_by_address(ep);
	if (pep->size == 0) {
		LOG_ERR("ep 0x%02x not configured", ep);
		return -EINVAL;
	}
	if (!pio_usb_ll_transfer_start(pep, buf, len)) {
		LOG_ERR("ll_transfer_start(0x%02x) refused", ep);
		return -EIO;
	}
	return 0;
}

/* ===== Setup packet routing ==========================================
 *
 * On receiving SETUP, our IRQ override has already snapshotted the bytes
 * into the controller's root->setup_packet. We pull them here, build a
 * net_buf via udc_ctrl_alloc(), and dispatch via the udc_ctrl_submit_*
 * helpers based on whether the request has a data stage and which
 * direction.
 */

static void handle_setup(const struct device *dev)
{
	root_port_t *root = PIO_USB_ROOT_PORT(0);
	struct net_buf *buf;

	buf = udc_ctrl_alloc(dev, USB_CONTROL_EP_OUT, 8);
	if (buf == NULL) {
		LOG_ERR("ENOMEM allocating SETUP buffer");
		return;
	}
	net_buf_add_mem(buf, root->setup_packet, 8);

	udc_ep_buf_set_setup(buf);
	udc_ctrl_update_stage(dev, buf);

	if (udc_ctrl_stage_is_data_out(dev)) {
		/* Control Write: prepare to receive data on EP0 OUT, then
		 * defer the upper-layer notification until the OUT data
		 * arrives (handled in handle_ep_complete for EP0). */
		const struct usb_setup_packet *setup =
			(const struct usb_setup_packet *)root->setup_packet;
		uint16_t wlen = sys_le16_to_cpu(setup->wLength);
		struct net_buf *dout = udc_ctrl_alloc(dev, USB_CONTROL_EP_OUT,
						      wlen);
		if (dout == NULL) {
			LOG_ERR("ENOMEM allocating EP0 OUT data buffer");
			return;
		}
		(void)kick_off_out(USB_CONTROL_EP_OUT, dout->data, wlen);
		/* The buffer is held by the upper layer — udc_ctrl_submit_s_out_status
		 * will be called when handle_ep_complete sees EP0 OUT done. */
	} else if (udc_ctrl_stage_is_data_in(dev)) {
		/* Control Read: upper layer will enqueue the IN buffer via
		 * ep_enqueue. Notify it now via udc_ctrl_submit_s_in_status. */
		(void)udc_ctrl_submit_s_in_status(dev);
	} else {
		/* No-data Control: status-in stage. */
		(void)udc_ctrl_submit_s_status(dev);
	}
}

/* ===== Endpoint completion =========================================== */

static void handle_ep_complete(const struct device *dev, uint8_t ep_idx)
{
	endpoint_t *pep = PIO_USB_ENDPOINT(ep_idx);
	uint8_t ep_addr = pep->ep_num | (pep->is_tx ? 0x80 : 0x00);

	struct net_buf *buf = udc_buf_get(dev, ep_addr);
	if (buf == NULL) {
		/* No upper-layer transfer queued (e.g. spontaneous SOF or
		 * EP0 status without an enqueued status buffer). Nothing
		 * to forward. */
		return;
	}

	if (USB_EP_DIR_IS_OUT(ep_addr) && pep->actual_len > 0) {
		net_buf_add(buf, pep->actual_len);
	}

	if (ep_addr == USB_CONTROL_EP_OUT) {
		if (udc_ctrl_stage_is_status_out(dev)) {
			udc_ctrl_update_stage(dev, buf);
			(void)udc_ctrl_submit_status(dev, buf);
		} else if (udc_ctrl_stage_is_data_out(dev)) {
			udc_ctrl_update_stage(dev, buf);
			(void)udc_ctrl_submit_s_out_status(dev, buf);
		} else {
			udc_submit_ep_event(dev, buf, 0);
		}
	} else if (ep_addr == USB_CONTROL_EP_IN) {
		if (udc_ctrl_stage_is_data_in(dev)) {
			udc_ctrl_update_stage(dev, buf);
		} else if (udc_ctrl_stage_is_status_in(dev)) {
			/* Status IN completed — apply any pending
			 * SET_ADDRESS now, after the host has acknowledged
			 * the response. */
			struct udc_pio_usb_data_priv *priv = priv_for(dev);
			if (priv->address_pending) {
				PIO_USB_ROOT_PORT(0)->dev_addr =
					priv->pending_address;
				priv->address_pending = false;
			}
			udc_ctrl_submit_status(dev, buf);
			udc_ctrl_update_stage(dev, buf);
		}
	} else {
		udc_submit_ep_event(dev, buf, 0);
	}
}

static void drain_ep_complete(const struct device *dev)
{
	root_port_t *root = PIO_USB_ROOT_PORT(0);
	uint32_t pending = root->ep_complete;
	root->ep_complete &= ~pending;
	while (pending) {
		uint8_t idx = __builtin_ctz(pending);
		pending &= ~(1u << idx);
		handle_ep_complete(dev, idx);
	}
}

/* ===== Driver thread ================================================= */

static void udc_pio_usb_thread(void *p1, void *p2, void *p3)
{
	const struct device *dev = p1;
	struct udc_pio_usb_data_priv *priv = priv_for(dev);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (!priv->stop) {
		k_sem_take(&priv->work_sem, K_MSEC(1));
		if (!priv->enabled) {
			continue;
		}

		atomic_val_t f = atomic_clear(&priv->flags);

		if (f & WORK_FLAG_CONNECT) {
			udc_submit_event(dev, UDC_EVT_VBUS_READY, 0);
		}
		if (f & WORK_FLAG_DISCONNECT) {
			udc_submit_event(dev, UDC_EVT_VBUS_REMOVED, 0);
		}
		if (f & WORK_FLAG_RESET) {
			udc_submit_event(dev, UDC_EVT_RESET, 0);
		}
		if (f & WORK_FLAG_SETUP) {
			handle_setup(dev);
		}
		if (f & WORK_FLAG_EP_DONE) {
			drain_ep_complete(dev);
		}
	}
}

/* ===== IRQ override ===================================================
 *
 * Strong override of the WEAK alias declared at the bottom of
 * pio_usb_device.c. The packet-level PIO RX IRQ
 * (usb_device_packet_handler) still runs and populates root->ints +
 * root->setup_packet + the EP receive buffers; we just intercept the
 * post-receive dispatch so the standard-request stack in
 * process_device_setup_stage() never runs.
 */

void pio_usb_device_irq_handler(uint8_t root_idx)
{
	root_port_t *root = PIO_USB_ROOT_PORT(root_idx);
	const uint32_t ints = root->ints;

	atomic_val_t set = 0;
	if (ints & PIO_USB_INTS_RESET_END_BITS) {
		set |= WORK_FLAG_RESET;
	}
	if (ints & PIO_USB_INTS_SETUP_REQ_BITS) {
		set |= WORK_FLAG_SETUP;
	}
	if (ints & PIO_USB_INTS_ENDPOINT_COMPLETE_BITS) {
		set |= WORK_FLAG_EP_DONE;
	}

	root->ints &= ~ints;

	if (set != 0 && udc_pio_usb_singleton != NULL) {
		struct udc_pio_usb_data_priv *priv =
			priv_for(udc_pio_usb_singleton);
		atomic_or(&priv->flags, set);
		k_sem_give(&priv->work_sem);
	}
}

/* ===== UDC API ======================================================== */

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
	const struct udc_pio_usb_cfg *cfg = dev->config;
	struct udc_pio_usb_data_priv *priv = priv_for(dev);

	if (udc_pio_usb_singleton != NULL && udc_pio_usb_singleton != dev) {
		LOG_ERR("udc_pio_usb supports a single instance");
		return -ENOTSUP;
	}

	pio_usb_configuration_t pio_cfg = PIO_USB_DEFAULT_CONFIG;
	pio_cfg.pin_dp = cfg->pin_dp;
	pio_cfg.pinout = cfg->pinout_dpdm ? PIO_USB_PINOUT_DPDM
					  : PIO_USB_PINOUT_DMDP;

	/* Pico-PIO-USB's device init copies *buffers into a static
	 * descriptor_buffers — the built-in standard-request handlers we're
	 * overriding read from it. Since our IRQ handler routes setup
	 * packets to Zephyr UDC instead, those handlers never run and the
	 * static stays zero-filled. */
	static const usb_descriptor_buffers_t empty_descriptors = {0};
	(void)pio_usb_device_init(&pio_cfg, &empty_descriptors);

	udc_pio_usb_singleton = dev;
	priv->initialized = true;
	priv->enabled = false;
	priv->stop = false;
	priv->address_pending = false;
	atomic_clear(&priv->flags);
	k_sem_init(&priv->work_sem, 0, 1);

	if (udc_ep_enable_internal(dev, USB_CONTROL_EP_OUT,
				   USB_EP_TYPE_CONTROL, 64, 0)) {
		LOG_ERR("Failed to enable control OUT");
		return -EIO;
	}
	if (udc_ep_enable_internal(dev, USB_CONTROL_EP_IN,
				   USB_EP_TYPE_CONTROL, 64, 0)) {
		LOG_ERR("Failed to enable control IN");
		return -EIO;
	}

	k_thread_create(&priv->thread, udc_pio_usb_thread_stack,
			K_KERNEL_STACK_SIZEOF(udc_pio_usb_thread_stack),
			udc_pio_usb_thread, (void *)dev, NULL, NULL,
			CONFIG_UDC_PIO_USB_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&priv->thread, "udc_pio_usb");

	LOG_INF("udc_pio_usb initialized (pin_dp=%u %s)", cfg->pin_dp,
		cfg->pinout_dpdm ? "DPDM" : "DMDP");
	return 0;
}

static int udc_pio_usb_enable(const struct device *dev)
{
	struct udc_pio_usb_data_priv *priv = priv_for(dev);
	priv->enabled = true;
	k_sem_give(&priv->work_sem);
	return 0;
}

static int udc_pio_usb_disable(const struct device *dev)
{
	struct udc_pio_usb_data_priv *priv = priv_for(dev);
	priv->enabled = false;
	return 0;
}

static int udc_pio_usb_shutdown(const struct device *dev)
{
	struct udc_pio_usb_data_priv *priv = priv_for(dev);

	priv->enabled = false;
	priv->stop = true;
	k_sem_give(&priv->work_sem);
	k_thread_join(&priv->thread, K_MSEC(100));

	(void)udc_ep_disable_internal(dev, USB_CONTROL_EP_OUT);
	(void)udc_ep_disable_internal(dev, USB_CONTROL_EP_IN);

	priv->initialized = false;
	udc_pio_usb_singleton = NULL;
	return 0;
}

static int udc_pio_usb_set_address(const struct device *dev, uint8_t addr)
{
	struct udc_pio_usb_data_priv *priv = priv_for(dev);
	/* USB protocol: address change takes effect after the STATUS IN
	 * for the SET_ADDRESS request. Defer here, apply in
	 * handle_ep_complete on EP0 IN status completion. */
	priv->pending_address = addr;
	priv->address_pending = true;
	return 0;
}

static int udc_pio_usb_host_wakeup(const struct device *dev)
{
	ARG_UNUSED(dev);
	/* Remote wake-up not supported by Pico-PIO-USB device mode. */
	return -ENOTSUP;
}

static enum udc_bus_speed udc_pio_usb_device_speed(const struct device *dev)
{
	ARG_UNUSED(dev);
	return UDC_BUS_SPEED_FS;
}

/* Build the descriptor that pio_usb_ll_configure_endpoint expects.
 * Mirrors the layout of pio_usb_device_endpoint_open() but lets us route
 * through the LL function without a hard dep on the upstream wrapper. */
struct ll_ep_desc {
	uint8_t length;
	uint8_t type;
	uint8_t epaddr;
	uint8_t attr;
	uint8_t max_size[2];
	uint8_t interval;
} __packed;

static uint8_t pico_attr_for_zephyr(uint8_t type)
{
	switch (type) {
	case USB_EP_TYPE_CONTROL:    return EP_ATTR_CONTROL;
	case USB_EP_TYPE_ISO:        return EP_ATTR_ISOCHRONOUS;
	case USB_EP_TYPE_BULK:       return EP_ATTR_BULK;
	case USB_EP_TYPE_INTERRUPT:  return EP_ATTR_INTERRUPT;
	default:                     return EP_ATTR_BULK;
	}
}

static int udc_pio_usb_ep_enable(const struct device *dev,
				 struct udc_ep_config *const cfg)
{
	ARG_UNUSED(dev);
	struct ll_ep_desc desc = {
		.length = sizeof(desc),
		.type = 5, /* USB_DESC_ENDPOINT */
		.epaddr = cfg->addr,
		.attr = pico_attr_for_zephyr(cfg->attributes & 0x03),
	};
	sys_put_le16(cfg->mps, desc.max_size);
	desc.interval = cfg->interval;

	endpoint_t *pep = pio_usb_device_get_endpoint_by_address(cfg->addr);
	pio_usb_ll_configure_endpoint(pep, (uint8_t *)&desc);
	pep->root_idx = 0;
	pep->dev_addr = 0;
	pep->need_pre = 0;
	pep->is_tx = USB_EP_DIR_IS_IN(cfg->addr);
	return 0;
}

static int udc_pio_usb_ep_disable(const struct device *dev,
				  struct udc_ep_config *const cfg)
{
	ARG_UNUSED(dev);
	endpoint_t *pep = pio_usb_device_get_endpoint_by_address(cfg->addr);
	pep->size = 0; /* upstream uses size==0 to mark unconfigured */
	return 0;
}

static int udc_pio_usb_ep_set_halt(const struct device *dev,
				   struct udc_ep_config *const cfg)
{
	ARG_UNUSED(dev);
	endpoint_t *pep = pio_usb_device_get_endpoint_by_address(cfg->addr);
	pep->stalled = true;
	cfg->stat.halted = true;
	return 0;
}

static int udc_pio_usb_ep_clear_halt(const struct device *dev,
				     struct udc_ep_config *const cfg)
{
	ARG_UNUSED(dev);
	endpoint_t *pep = pio_usb_device_get_endpoint_by_address(cfg->addr);
	pep->stalled = false;
	cfg->stat.halted = false;
	return 0;
}

static int udc_pio_usb_ep_enqueue(const struct device *dev,
				  struct udc_ep_config *const cfg,
				  struct net_buf *buf)
{
	ARG_UNUSED(dev);
	udc_buf_put(cfg, buf);

	if (cfg->stat.halted) {
		return 0; /* will be retriggered after clear_halt */
	}

	uint8_t *data = buf->data;
	uint16_t len = USB_EP_DIR_IS_IN(cfg->addr) ? buf->len
						   : net_buf_tailroom(buf);
	if (USB_EP_DIR_IS_IN(cfg->addr)) {
		return kick_off_in(cfg->addr, data, len);
	}
	return kick_off_out(cfg->addr, data, len);
}

static int udc_pio_usb_ep_dequeue(const struct device *dev,
				  struct udc_ep_config *const cfg)
{
	struct net_buf *buf;
	unsigned int key = irq_lock();

	buf = udc_buf_get_all(dev, cfg->addr);
	if (buf != NULL) {
		udc_submit_ep_event(dev, buf, -ECONNABORTED);
	}

	endpoint_t *pep = pio_usb_device_get_endpoint_by_address(cfg->addr);
	pep->has_transfer = false;
	pep->transfer_started = false;

	irq_unlock(key);
	return 0;
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

/* ===== Pre-init + device instantiation =============================== */

static int udc_pio_usb_driver_preinit(const struct device *dev)
{
	const struct udc_pio_usb_cfg *cfg = dev->config;
	struct udc_data *data = dev->data;

	k_mutex_init(&data->mutex);
	data->caps.mps0 = UDC_MPS0_64;
	data->caps.hs = false;

	for (int i = 0; i < UDC_PIO_USB_NUM_EPS; i++) {
		cfg->ep_cfg_out[i].caps.out = 1;
		if (i == 0) {
			cfg->ep_cfg_out[i].caps.control = 1;
			cfg->ep_cfg_out[i].caps.mps = 64;
		} else {
			cfg->ep_cfg_out[i].caps.bulk = 1;
			cfg->ep_cfg_out[i].caps.interrupt = 1;
			cfg->ep_cfg_out[i].caps.mps = 64;
		}
		cfg->ep_cfg_out[i].addr = USB_EP_DIR_OUT | i;
		(void)udc_register_ep(dev, &cfg->ep_cfg_out[i]);

		cfg->ep_cfg_in[i].caps.in = 1;
		if (i == 0) {
			cfg->ep_cfg_in[i].caps.control = 1;
			cfg->ep_cfg_in[i].caps.mps = 64;
		} else {
			cfg->ep_cfg_in[i].caps.bulk = 1;
			cfg->ep_cfg_in[i].caps.interrupt = 1;
			cfg->ep_cfg_in[i].caps.mps = 64;
		}
		cfg->ep_cfg_in[i].addr = USB_EP_DIR_IN | i;
		(void)udc_register_ep(dev, &cfg->ep_cfg_in[i]);
	}

	return 0;
}

#define UDC_PIO_USB_DEVICE_DEFINE(n)                                          \
	static struct udc_ep_config udc_pio_usb_ep_in_##n[UDC_PIO_USB_NUM_EPS]; \
	static struct udc_ep_config udc_pio_usb_ep_out_##n[UDC_PIO_USB_NUM_EPS]; \
	static const struct udc_pio_usb_cfg udc_pio_usb_cfg_##n = {           \
		.ep_cfg_in = udc_pio_usb_ep_in_##n,                           \
		.ep_cfg_out = udc_pio_usb_ep_out_##n,                         \
		.pin_dp = DT_INST_PROP(n, pin_dp),                            \
		.pinout_dpdm =                                                \
			DT_INST_ENUM_IDX_OR(n, pinout, 0) == 0,               \
	};                                                                    \
	static struct udc_pio_usb_data_priv udc_pio_usb_priv_##n;             \
	static struct udc_data udc_pio_usb_data_##n = {                       \
		.mutex = Z_MUTEX_INITIALIZER(udc_pio_usb_data_##n.mutex),     \
		.priv = &udc_pio_usb_priv_##n,                                \
	};                                                                    \
	DEVICE_DT_INST_DEFINE(n, udc_pio_usb_driver_preinit, NULL,            \
			      &udc_pio_usb_data_##n,                          \
			      &udc_pio_usb_cfg_##n,                           \
			      POST_KERNEL,                                    \
			      CONFIG_KERNEL_INIT_PRIORITY_DEVICE,             \
			      &udc_pio_usb_api);

DT_INST_FOREACH_STATUS_OKAY(UDC_PIO_USB_DEVICE_DEFINE)

BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) <= 1,
	     "udc_pio_usb supports a single DT instance");
