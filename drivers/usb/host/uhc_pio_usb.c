/*
 * Copyright (c) 2026 The Cosmos-Keyboards Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Zephyr UHC (USB Host Controller) driver wrapping sekigon-gonnoc/Pico-PIO-USB
 * in host mode. See README.md "Architecture" for why we wrap the
 * pio_usb_host_* layer here (vs the LL layer, which the UDC driver uses).
 *
 * Threading model:
 *
 *   Pico-PIO-USB normally uses an alarm-pool repeating timer to call
 *   pio_usb_host_frame() every 1 ms. Zephyr's hal_rpi_pico doesn't ship
 *   pico_time, so we set skip_alarm_pool=true at init and run our own
 *   1 ms work item. The work item:
 *     - calls pio_usb_host_frame() (sends SOF, processes queued
 *       transfers, polls connect/disconnect, fires the IRQ handler)
 *     - drains root_port_t->event into UHC_EVT_DEV_CONNECTED_*
 *       / UHC_EVT_DEV_REMOVED
 *     - drains the per-endpoint completion bitmaps into the in-flight
 *       transfer table and calls uhc_xfer_return for any that finished
 *
 *   Higher-layer ep_enqueue is non-blocking: it appends to the UHC
 *   transfer list and signals the work item. Actual packet motion
 *   happens inside pio_usb_host_frame() at the next 1 ms tick.
 *
 * Endpoint lifecycle:
 *
 *   Zephyr UHC has no "open endpoint" callback — endpoints are implicit
 *   in the (addr, ep) pair carried on each xfer. Pico-PIO-USB requires
 *   pio_usb_host_endpoint_open() before the first transfer to allocate
 *   a slot in its EP pool. We open lazily on first use, building a
 *   minimal endpoint_descriptor_t from xfer->mps + xfer->attrib.
 *
 * Status: first-iteration code, NOT yet built or run on hardware.
 */

#define DT_DRV_COMPAT raspberrypi_pio_usb_host

#include <zephyr/device.h>
#include <zephyr/drivers/usb/uhc.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/usb/usb_ch9.h>

/* uhc_common.h is a private header in Zephyr's drivers/usb/uhc/ — pulled in
 * via the include path added in zephyr/CMakeLists.txt. */
#include "uhc_common.h"

/* Pico-PIO-USB headers (from the upstream module via west import).
 * pio_usb_ll.h declares the host endpoint functions, root-port macros,
 * and EP pool macros — pio_usb.h alone only exports init/task/stop. */
#include <hardware/structs/timer.h>
#include "pio_usb.h"
#include "pio_usb_configuration.h"
#include "pio_usb_ll.h"
#include "usb_definitions.h"

/* Strong override of Pico-PIO-USB's pio_usb_host_irq_handler weak
 * alias. Same trick udc_pio_usb uses for the device side.
 *
 * The upstream __pio_usb_host_irq_handler calls handle_endpoint_irq
 * which iterates root->ep_complete/error/stalled, looks up each
 * endpoint's owning device in pio_usb_device[], processes only
 * completions for matched devices, and UNCONDITIONALLY CLEARS all
 * bits at the end — even when no device was matched. We don't
 * populate pio_usb_device[] (Zephyr's USB host stack tracks
 * devices, not Pico-PIO-USB), so the upstream handler swallows
 * every completion bit before our drain_endpoint_bitmap can see it.
 *
 * Our override only translates connect/disconnect ints into
 * root->event (so surface_root_events can route them to the UHC
 * stack) and leaves the per-endpoint bits intact for our driver
 * thread's drain pass. */
void pio_usb_host_irq_handler(uint8_t root_id)
{
	root_port_t *root = PIO_USB_ROOT_PORT(root_id);
	const uint32_t ints = root->ints;

	if (ints & PIO_USB_INTS_CONNECT_BITS) {
		root->event = EVENT_CONNECT;
	}
	if (ints & PIO_USB_INTS_DISCONNECT_BITS) {
		root->event = EVENT_DISCONNECT;
	}

	/* Clear only the connect/disconnect bits; leave ENDPOINT_COMPLETE
	 * / STALLED / ERROR bits (and their per-endpoint counterparts in
	 * ep_complete/ep_stalled/ep_error) for our drain pass. */
	root->ints &= ~(PIO_USB_INTS_CONNECT_BITS | PIO_USB_INTS_DISCONNECT_BITS);
}

LOG_MODULE_REGISTER(uhc_pio_usb, CONFIG_UHC_DRIVER_LOG_LEVEL);

/* PICO_NO_HARDWARE / __no_inline_not_in_flash_func / __not_in_flash etc come
 * from <pico/platform.h>, pulled in transitively via pio_usb.h. */

/* ===== Per-instance config / runtime state ============================ */

/* Mirrors the endpoint_descriptor_t layout that pio_usb_host_endpoint_open
 * expects (it casts the pointer to (endpoint_descriptor_t *) and reads
 * .epaddr / .attr / .max_size / .interval). The full type lives in
 * usb_definitions.h; we redeclare the needed fields locally so the layout
 * is obvious in this file. */
struct uhc_pio_usb_ep_desc {
	uint8_t length;
	uint8_t type;
	uint8_t epaddr;
	uint8_t attr;
	uint8_t max_size[2];
	uint8_t interval;
} __packed;

#define UHC_PIO_USB_MAX_DEVICES 4 /* matches PIO_USB_DEVICE_CNT */

/* In-flight transfer slot: pairs a Zephyr xfer with the (dev, ep) it owns
 * inside Pico-PIO-USB's endpoint pool. */
struct uhc_pio_usb_inflight {
	struct uhc_transfer *xfer;
	uint8_t dev_addr;
	uint8_t ep_addr;
	bool open;     /* endpoint has been opened in Pico-PIO-USB's EP pool */
	bool started;  /* transfer has been kicked off (SETUP or DATA stage) */
};

struct uhc_pio_usb_data_priv {
	struct k_work_delayable frame_work;
	struct k_thread thread;
	struct k_sem queue_sem;
	bool initialized;
	bool enabled;
	bool stop;

	/* Slot per UHC transfer in flight. UHC's transfer queue is owned
	 * by uhc_common; we just need to track a small amount of per-xfer
	 * state for the 1ms loop. One slot per device address is plenty
	 * for the split-USB use case. */
	struct uhc_pio_usb_inflight inflight[UHC_PIO_USB_MAX_DEVICES * 4];

	/* Last known root_port_t->event so we don't double-fire connect/
	 * disconnect events. Pico-PIO-USB sets this in
	 * pio_usb_host_frame()'s connection-check pass. */
	usb_device_event_t last_root_event;
};

struct uhc_pio_usb_cfg {
	uint8_t pin_dp;          /* GPIO number of the D+ pin */
	bool pinout_dpdm;        /* true = DM = DP+1, false = DM = DP-1 */
	bool low_speed;          /* prefer low-speed bus (default full-speed) */
};

K_KERNEL_STACK_DEFINE(uhc_pio_usb_thread_stack, CONFIG_UHC_PIO_USB_THREAD_STACK_SIZE);

/* Pico-PIO-USB owns one root port per init call. We support exactly one
 * uhc-pio-usb DT instance — multi-instance would need a re-architect of
 * Pico-PIO-USB's static state. */
static const struct device *uhc_pio_usb_singleton;

/* ===== Helpers ======================================================== */

static struct uhc_pio_usb_inflight *
slot_for_xfer(struct uhc_pio_usb_data_priv *priv, struct uhc_transfer *xfer)
{
	for (size_t i = 0; i < ARRAY_SIZE(priv->inflight); i++) {
		if (priv->inflight[i].xfer == xfer) {
			return &priv->inflight[i];
		}
	}
	return NULL;
}

static struct uhc_pio_usb_inflight *
slot_alloc(struct uhc_pio_usb_data_priv *priv, struct uhc_transfer *xfer)
{
	for (size_t i = 0; i < ARRAY_SIZE(priv->inflight); i++) {
		if (priv->inflight[i].xfer == NULL) {
			priv->inflight[i] = (struct uhc_pio_usb_inflight){
				.xfer = xfer,
				.dev_addr = xfer->addr,
				.ep_addr = xfer->ep,
				.open = false,
				.started = false,
			};
			return &priv->inflight[i];
		}
	}
	return NULL;
}

static void slot_free(struct uhc_pio_usb_inflight *slot)
{
	memset(slot, 0, sizeof(*slot));
}

/* Build a minimal endpoint descriptor from a UHC transfer so
 * pio_usb_host_endpoint_open can carve out an EP pool slot. */
static void build_ep_desc(const struct uhc_transfer *xfer,
			  uint8_t ep_addr,
			  uint8_t attr,
			  struct uhc_pio_usb_ep_desc *out)
{
	out->length = sizeof(*out);
	out->type = 5; /* USB_DESC_ENDPOINT */
	out->epaddr = ep_addr;
	out->attr = attr;
	sys_put_le16(xfer->mps, out->max_size);
	out->interval = (uint8_t)xfer->timeout;
}

/* Translate a Zephyr UHC transfer's (ep, attrib) into a Pico-PIO-USB
 * endpoint type byte (EP_ATTR_CONTROL/BULK/INTERRUPT/ISOCHRONOUS). */
static uint8_t pico_attr_for_xfer(const struct uhc_transfer *xfer)
{
	uint8_t ep_idx = xfer->ep & 0x0f;
	if (ep_idx == 0) {
		return EP_ATTR_CONTROL;
	}
	/* xfer->attrib is opaque per the UHC API but in practice carries
	 * USB_EP_TYPE_* (matches pico's EP_ATTR_*). Default to BULK if
	 * unknown — most non-EP0 traffic is bulk for the split-USB use
	 * case. */
	uint8_t a = xfer->attrib & 0x03;
	if (a == 0 && ep_idx != 0) {
		return EP_ATTR_BULK;
	}
	return a;
}

static bool ensure_endpoint_open(struct uhc_pio_usb_inflight *slot,
				 const struct uhc_transfer *xfer)
{
	if (slot->open) {
		return true;
	}
	struct uhc_pio_usb_ep_desc desc;
	build_ep_desc(xfer, slot->ep_addr, pico_attr_for_xfer(xfer), &desc);

	if (!pio_usb_host_endpoint_open(0, slot->dev_addr,
					(const uint8_t *)&desc, false)) {
		LOG_ERR("endpoint_open failed (dev=%u ep=0x%02x)",
			slot->dev_addr, slot->ep_addr);
		return false;
	}
	slot->open = true;
	return true;
}

/* ===== Frame work: drives Pico-PIO-USB and drains completion state ==== */

static void surface_root_events(const struct device *dev,
				struct uhc_pio_usb_data_priv *priv)
{
	root_port_t *root = PIO_USB_ROOT_PORT(0);
	if (root->event == priv->last_root_event) {
		return;
	}
	priv->last_root_event = root->event;

	switch (root->event) {
	case EVENT_CONNECT:
		LOG_DBG("device connected (%s-speed)",
			root->is_fullspeed ? "full" : "low");
		uhc_submit_event(dev,
				 root->is_fullspeed
					 ? UHC_EVT_DEV_CONNECTED_FS
					 : UHC_EVT_DEV_CONNECTED_LS,
				 0);
		break;
	case EVENT_DISCONNECT:
		LOG_DBG("device disconnected");
		uhc_submit_event(dev, UHC_EVT_DEV_REMOVED, 0);
		/* Free any in-flight slots — Pico-PIO-USB's connection_check
		 * has already marked them complete with PIO_USB_INTS_ENDPOINT_ERROR_BITS
		 * but we also need to release our slot table. */
		for (size_t i = 0; i < ARRAY_SIZE(priv->inflight); i++) {
			struct uhc_pio_usb_inflight *s = &priv->inflight[i];
			if (s->xfer != NULL) {
				uhc_xfer_return(dev, s->xfer, -ENODEV);
				slot_free(s);
			}
		}
		break;
	default:
		break;
	}
}

/* Find the in-flight slot whose Pico-PIO-USB endpoint matches a flagged
 * EP pool index, then surface a UHC completion. */
static void drain_endpoint_bitmap(const struct device *dev,
				  struct uhc_pio_usb_data_priv *priv,
				  volatile uint32_t *ep_reg,
				  int err)
{
	uint32_t pending = *ep_reg;
	if (pending) {
		printk("[uhc] drain_endpoint_bitmap pending=0x%08x err=%d\n",
		       pending, err);
	}
	*ep_reg &= ~pending;

	while (pending) {
		uint8_t ep_idx = __builtin_ctz(pending);
		pending &= ~(1u << ep_idx);

		endpoint_t *ep = PIO_USB_ENDPOINT(ep_idx);
		if (ep->size == 0) {
			continue;
		}

		struct uhc_pio_usb_inflight *match = NULL;
		for (size_t i = 0; i < ARRAY_SIZE(priv->inflight); i++) {
			struct uhc_pio_usb_inflight *s = &priv->inflight[i];
			if (s->xfer != NULL && s->dev_addr == ep->dev_addr &&
			    (s->ep_addr & 0x7f) == (ep->ep_num & 0x7f)) {
				match = s;
				break;
			}
		}
		if (match == NULL) {
			continue;
		}

		/* Update buffer length on the xfer's net_buf if data came in
		 * (control IN data or bulk IN). Pico-PIO-USB stores the
		 * actual transferred length in ep->actual_len. */
		if (err == 0 && match->xfer->buf != NULL &&
		    USB_EP_DIR_IS_IN(match->xfer->ep)) {
			net_buf_add(match->xfer->buf, ep->actual_len);
		}

		uhc_xfer_return(dev, match->xfer, err);
		slot_free(match);
	}
}

static int kick_off_xfer(struct uhc_pio_usb_inflight *slot,
			 struct uhc_transfer *xfer)
{
	printk("[uhc] kick_off_xfer dev=%u ep=0x%02x stage=%u\n",
	       slot->dev_addr, slot->ep_addr, xfer->stage);
	if (!ensure_endpoint_open(slot, xfer)) {
		printk("[uhc] ensure_endpoint_open FAILED\n");
		return -EIO;
	}
	printk("[uhc] endpoint open OK\n");

	uint8_t ep_idx = xfer->ep & 0x0f;
	if (ep_idx == 0) {
		/* Control transfer — stage tells us whether to send SETUP
		 * or chase the data/status. UHC layer drives stage
		 * advancement; we just emit the right packet for the
		 * current stage. */
		switch (xfer->stage) {
		case UHC_CONTROL_STAGE_SETUP:
			printk("[uhc] sending SETUP packet\n");
			if (!pio_usb_host_send_setup(0, slot->dev_addr,
						     xfer->setup_pkt)) {
				printk("[uhc] pio_usb_host_send_setup FAILED\n");
				return -EIO;
			}
			printk("[uhc] SETUP queued\n");
			break;
		case UHC_CONTROL_STAGE_DATA:
			if (!pio_usb_host_endpoint_transfer(
				    0, slot->dev_addr, xfer->ep,
				    xfer->buf ? xfer->buf->data : NULL,
				    xfer->buf ? net_buf_tailroom(xfer->buf)
					      : 0)) {
				return -EIO;
			}
			break;
		case UHC_CONTROL_STAGE_STATUS:
			/* STATUS is a zero-length transfer in the opposite
			 * direction from DATA (or IN if no DATA stage). */
			if (!pio_usb_host_endpoint_transfer(
				    0, slot->dev_addr,
				    USB_EP_DIR_IS_IN(xfer->ep) ? 0x00 : 0x80,
				    NULL, 0)) {
				return -EIO;
			}
			break;
		}
	} else {
		/* Bulk / interrupt — single transaction. */
		uint8_t *buf = xfer->buf ? xfer->buf->data : NULL;
		uint16_t len = xfer->buf
				       ? (USB_EP_DIR_IS_IN(xfer->ep)
						  ? net_buf_tailroom(xfer->buf)
						  : xfer->buf->len)
				       : 0;
		if (!pio_usb_host_endpoint_transfer(0, slot->dev_addr,
						    xfer->ep, buf, len)) {
			return -EIO;
		}
	}

	slot->started = true;
	return 0;
}

static void process_pending_xfers(const struct device *dev,
				  struct uhc_pio_usb_data_priv *priv)
{
	struct uhc_transfer *xfer;
	while ((xfer = uhc_xfer_get_next(dev)) != NULL) {
		struct uhc_pio_usb_inflight *slot = slot_for_xfer(priv, xfer);
		if (slot == NULL) {
			slot = slot_alloc(priv, xfer);
			if (slot == NULL) {
				LOG_WRN("inflight table full, deferring xfer");
				return;
			}
		}
		if (slot->started) {
			continue;
		}
		int ret = kick_off_xfer(slot, xfer);
		if (ret) {
			uhc_xfer_return(dev, xfer, ret);
			slot_free(slot);
		}
	}
}

static void uhc_pio_usb_thread(void *p1, void *p2, void *p3)
{
	const struct device *dev = p1;
	struct uhc_pio_usb_data_priv *priv = uhc_get_private(dev);
	root_port_t *root = PIO_USB_ROOT_PORT(0);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	uint32_t loop_count = 0;
	while (!priv->stop) {
		/* Wait at most 1 ms — long enough to behave like the
		 * upstream SOF cadence, short enough to react to ep_enqueue
		 * promptly. */
		k_sem_take(&priv->queue_sem, K_MSEC(1));

		if (!priv->enabled) {
			continue;
		}

		/* Print on every iteration once any transfer has been kicked
		 * off so we can see exactly which call hangs the loop, and
		 * a heartbeat every 50 ticks otherwise. */
		endpoint_t *e0 = PIO_USB_ENDPOINT(0);
		bool any_started = false;
		for (size_t i = 0; i < ARRAY_SIZE(priv->inflight); i++) {
			if (priv->inflight[i].started) {
				any_started = true;
				break;
			}
		}
		bool probe = any_started || ((loop_count % 50) == 0);
		if (probe) {
			printk("[uhc] iter=%u t=%u ints=0x%x epc=0x%x epe=0x%x ep0(s=%u n=0x%02x d=0x%02x h=%u st=%u f=%u)\n",
			       loop_count, timer_hw->timerawl,
			       root->ints, root->ep_complete, root->ep_error,
			       e0->size, e0->ep_num, e0->data_id,
			       e0->has_transfer, e0->transfer_started,
			       e0->failed_count);
		}
		loop_count++;

		/* Pico-PIO-USB normally calls pio_usb_host_frame() from its
		 * SOF timer; since we set skip_alarm_pool=true, we drive it
		 * here. This sends SOF, processes queued endpoint
		 * transactions, runs the connection-check pass, and invokes
		 * pio_usb_host_irq_handler for any flagged root ports. */
		if (probe) {
			printk("[uhc] frame_in\n");
		}
		pio_usb_host_frame();
		if (probe) {
			printk("[uhc] frame_out\n");
		}

		/* Surface connect/disconnect and per-endpoint completions
		 * back through the UHC event API. */
		surface_root_events(dev, priv);

		drain_endpoint_bitmap(dev, priv, &root->ep_complete, 0);
		drain_endpoint_bitmap(dev, priv, &root->ep_stalled, -EPIPE);
		drain_endpoint_bitmap(dev, priv, &root->ep_error, -EIO);

		/* Pump any newly enqueued transfers into Pico-PIO-USB. */
		process_pending_xfers(dev, priv);
	}
}

/* ===== UHC API ======================================================== */

static int uhc_pio_usb_lock(const struct device *dev)
{
	return uhc_lock_internal(dev, K_FOREVER);
}

static int uhc_pio_usb_unlock(const struct device *dev)
{
	return uhc_unlock_internal(dev);
}

static int uhc_pio_usb_init(const struct device *dev)
{
	const struct uhc_pio_usb_cfg *cfg = dev->config;
	struct uhc_pio_usb_data_priv *priv = uhc_get_private(dev);

	if (uhc_pio_usb_singleton != NULL && uhc_pio_usb_singleton != dev) {
		LOG_ERR("uhc_pio_usb supports a single instance");
		return -ENOTSUP;
	}
	uhc_pio_usb_singleton = dev;

	/* Pico-PIO-USB owns the D+/D- pins itself (gpio_set_function +
	 * gpio_pull_*); we don't touch pinctrl. */

	pio_usb_configuration_t pio_cfg = PIO_USB_DEFAULT_CONFIG;
	pio_cfg.pin_dp = cfg->pin_dp;
	pio_cfg.pinout = cfg->pinout_dpdm ? PIO_USB_PINOUT_DPDM
					  : PIO_USB_PINOUT_DMDP;
	pio_cfg.skip_alarm_pool = true;

	(void)pio_usb_host_init(&pio_cfg);

	/* pio_usb_host_init configures the PIO state machines but doesn't
	 * mark any root port as initialized — pio_usb_host_frame's
	 * connection-detection loop only runs for ports with
	 * root->initialized == true. Add the root port now so the bus
	 * actually starts polling for connect events. */
	if (pio_usb_host_add_port(cfg->pin_dp,
				  cfg->pinout_dpdm ? PIO_USB_PINOUT_DPDM
						   : PIO_USB_PINOUT_DMDP) != 0) {
		LOG_ERR("pio_usb_host_add_port failed");
		return -EIO;
	}

	priv->initialized = true;
	priv->enabled = false;
	priv->stop = false;
	priv->last_root_event = EVENT_NONE;
	memset(priv->inflight, 0, sizeof(priv->inflight));
	k_sem_init(&priv->queue_sem, 0, 1);

	k_thread_create(&priv->thread, uhc_pio_usb_thread_stack,
			K_KERNEL_STACK_SIZEOF(uhc_pio_usb_thread_stack),
			uhc_pio_usb_thread, (void *)dev, NULL, NULL,
			CONFIG_UHC_PIO_USB_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&priv->thread, "uhc_pio_usb");

	LOG_INF("uhc_pio_usb initialized (pin_dp=%u %s)", cfg->pin_dp,
		cfg->pinout_dpdm ? "DPDM" : "DMDP");
	return 0;
}

/* NOTE: we don't call pio_usb_host_restart / pio_usb_host_stop here.
 * Both busy-wait on flags (start_timer_flag / cancel_timer_flag) that
 * are only cleared by Pico-PIO-USB's SOF timer callback — but we
 * stubbed out the alarm pool, so that callback never runs and the
 * busy-wait would hang forever. Our driver thread directly calls
 * pio_usb_host_frame() and gates SOF generation on priv->enabled, so
 * we don't need the upstream start/stop dance. */

static int uhc_pio_usb_enable(const struct device *dev)
{
	struct uhc_pio_usb_data_priv *priv = uhc_get_private(dev);

	if (!priv->initialized) {
		return -EPERM;
	}
	priv->enabled = true;
	k_sem_give(&priv->queue_sem);
	return 0;
}

static int uhc_pio_usb_disable(const struct device *dev)
{
	struct uhc_pio_usb_data_priv *priv = uhc_get_private(dev);

	priv->enabled = false;
	return 0;
}

static int uhc_pio_usb_shutdown(const struct device *dev)
{
	struct uhc_pio_usb_data_priv *priv = uhc_get_private(dev);

	priv->enabled = false;
	priv->stop = true;
	k_sem_give(&priv->queue_sem);
	k_thread_join(&priv->thread, K_MSEC(100));
	priv->initialized = false;
	uhc_pio_usb_singleton = NULL;
	return 0;
}

static int uhc_pio_usb_bus_reset(const struct device *dev)
{
	pio_usb_host_port_reset_start(0);
	k_msleep(50);
	pio_usb_host_port_reset_end(0);
	uhc_submit_event(dev, UHC_EVT_RESETED, 0);
	return 0;
}

static int uhc_pio_usb_sof_enable(const struct device *dev)
{
	ARG_UNUSED(dev);
	/* Pico-PIO-USB sends SOF every frame whenever the bus is connected
	 * and not suspended; nothing to gate here. */
	return 0;
}

static int uhc_pio_usb_bus_suspend(const struct device *dev)
{
	ARG_UNUSED(dev);
	root_port_t *root = PIO_USB_ROOT_PORT(0);
	root->suspended = true;
	uhc_submit_event(dev, UHC_EVT_SUSPENDED, 0);
	return 0;
}

static int uhc_pio_usb_bus_resume(const struct device *dev)
{
	ARG_UNUSED(dev);
	root_port_t *root = PIO_USB_ROOT_PORT(0);
	root->suspended = false;
	uhc_submit_event(dev, UHC_EVT_RESUMED, 0);
	return 0;
}

static int uhc_pio_usb_ep_enqueue(const struct device *dev,
				  struct uhc_transfer *const xfer)
{
	struct uhc_pio_usb_data_priv *priv = uhc_get_private(dev);
	int ret = uhc_xfer_append(dev, xfer);
	if (ret == 0) {
		k_sem_give(&priv->queue_sem);
	}
	return ret;
}

static int uhc_pio_usb_ep_dequeue(const struct device *dev,
				  struct uhc_transfer *const xfer)
{
	struct uhc_pio_usb_data_priv *priv = uhc_get_private(dev);
	struct uhc_pio_usb_inflight *slot = slot_for_xfer(priv, xfer);

	if (slot != NULL && slot->started) {
		(void)pio_usb_host_endpoint_abort_transfer(0, slot->dev_addr,
							   slot->ep_addr);
	}
	if (slot != NULL) {
		slot_free(slot);
	}
	return 0;
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

/* ===== Device instantiation =========================================== */

/* Devicetree: pin-dp is the D+ GPIO number; pinout selects the D- pin
 * relative to D+ (dpdm = D- = D+ + 1, dmdp = D- = D+ - 1); data-rate
 * defaults to full-speed. */
#define UHC_PIO_USB_DEVICE_DEFINE(n)                                          \
	static const struct uhc_pio_usb_cfg uhc_pio_usb_cfg_##n = {           \
		.pin_dp = DT_INST_PROP(n, pin_dp),                            \
		.pinout_dpdm =                                                \
			DT_INST_ENUM_IDX_OR(n, pinout, 0) == 0,               \
		.low_speed =                                                  \
			DT_INST_ENUM_IDX_OR(n, data_rate, 0) == 1,            \
	};                                                                    \
	static struct uhc_pio_usb_data_priv uhc_pio_usb_priv_##n;             \
	static struct uhc_data uhc_pio_usb_data_##n = {                       \
		.mutex = Z_MUTEX_INITIALIZER(uhc_pio_usb_data_##n.mutex),     \
		.priv = &uhc_pio_usb_priv_##n,                                \
	};                                                                    \
	DEVICE_DT_INST_DEFINE(n, NULL, NULL,                                  \
			      &uhc_pio_usb_data_##n,                          \
			      &uhc_pio_usb_cfg_##n,                           \
			      POST_KERNEL,                                    \
			      CONFIG_KERNEL_INIT_PRIORITY_DEVICE,             \
			      &uhc_pio_usb_api);

DT_INST_FOREACH_STATUS_OKAY(UHC_PIO_USB_DEVICE_DEFINE)

BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) <= 1,
	     "uhc_pio_usb supports a single DT instance");
