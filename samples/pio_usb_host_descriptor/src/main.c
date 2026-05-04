/*
 * Copyright (c) 2026 The Cosmos-Keyboards Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Pico-PIO-USB host descriptor-dump sample. Brings up uhc_pio_usb on a
 * Lemon Wired (or any RP2040 board with a raspberrypi,pio-usb-host DT
 * node), enumerates whatever device gets plugged into the configured
 * Link USB-C port, and prints its device descriptor + manufacturer/
 * product/serial-number strings to CDC ACM on the native USB-C port.
 *
 * Modeled on tinyusb's dual/host_info_to_device_cdc example, but driven
 * through Zephyr's UHC API rather than tinyusb's host stack.
 *
 * Open the resulting tty (/dev/cu.usbmodem* on macOS, /dev/ttyACM0 on
 * Linux) at any baud. Boot logs are gated on DTR — the serial console
 * sees the full enumeration trace from the moment it attaches.
 *
 * Flow on UHC_EVT_DEV_CONNECTED_*:
 *
 *   1. uhc_bus_reset, then sleep ≥10ms (USB §9.2.6.3).
 *   2. GET_DESCRIPTOR(DEVICE, 8 bytes) at addr=0 — learn bMaxPacketSize0.
 *   3. SET_ADDRESS(1).
 *   4. GET_DESCRIPTOR(DEVICE, 18 bytes) at addr=1.
 *   5. GET_DESCRIPTOR(STRING, 0, 4) → langid.
 *   6. GET_DESCRIPTOR(STRING, iManufacturer/iProduct/iSerialNumber).
 *   7. Pretty-print everything.
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/usb/uhc.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/buf.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/usb/usb_ch9.h>

LOG_MODULE_REGISTER(pio_usb_host_descriptor_sample, LOG_LEVEL_INF);

#define UHC_NODE     DT_NODELABEL(pio_usb_host)
#define CONSOLE_NODE DT_CHOSEN(zephyr_console)

static const struct device *uhc_dev = DEVICE_DT_GET(UHC_NODE);
static const struct device *console = DEVICE_DT_GET(CONSOLE_NODE);

#define ENUM_ADDR  1
#define DEFAULT_MPS0 8
#define DESC_BUF_SIZE 256

/* Net-buf pool for the descriptor DATA stages. uhc_pio_usb does not
 * implement uhc_xfer_buf_alloc, so we BYO. Two buffers is enough — we
 * issue one transfer at a time and free between issues. */
NET_BUF_POOL_FIXED_DEFINE(desc_pool, 2, DESC_BUF_SIZE, 0, NULL);

/* UHC connect/transfer signaling. The UHC event callback runs in the
 * driver thread, so we just flag from there and do all the work in main. */
static atomic_t connected;
static struct k_sem connect_sem;
static struct k_sem xfer_done;

/* ===== UHC event callback =========================================== */

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
		atomic_set(&connected, 1);
		k_sem_give(&connect_sem);
		break;
	case UHC_EVT_DEV_REMOVED:
		LOG_INF("Device removed");
		atomic_clear(&connected);
		break;
	case UHC_EVT_EP_REQUEST:
		k_sem_give(&xfer_done);
		break;
	case UHC_EVT_RESETED:
		LOG_DBG("Bus reset complete");
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

/* ===== Wait for serial console ====================================== */

/* Block until the host opens the CDC ACM port (asserts DTR) so the
 * enumeration trace isn't dropped into a buffer that nobody's reading.
 * Time out after ~3s so the firmware still does work even if no console
 * is attached. */
static void wait_for_console(void)
{
	uint32_t dtr = 0;
	for (int i = 0; i < 30; i++) {
		uart_line_ctrl_get(console, UART_LINE_CTRL_DTR, &dtr);
		if (dtr) {
			break;
		}
		k_msleep(100);
	}
	k_msleep(200);
}

/* ===== Control-transfer helper ===================================== */

/* Issue a control request as a single uhc_transfer. The driver advances
 * SETUP→DATA→STATUS internally and only signals completion at the end —
 * matching the in-tree max3421e UHC pattern. xfer.ep encodes the DATA
 * direction; STATUS direction is implied as the opposite. */
static int issue_setup(uint8_t addr, uint16_t mps,
		       const struct usb_setup_packet *setup,
		       struct net_buf *data_buf)
{
	bool data_in = setup->RequestType.direction == USB_REQTYPE_DIR_TO_HOST;
	struct uhc_transfer xfer = {
		.addr = addr,
		.ep = data_in ? USB_CONTROL_EP_IN : USB_CONTROL_EP_OUT,
		.attrib = USB_EP_TYPE_CONTROL,
		.mps = mps,
		.timeout = 1000,
		.buf = data_buf,
		.stage = UHC_CONTROL_STAGE_SETUP,
	};
	memcpy(xfer.setup_pkt, setup, sizeof(xfer.setup_pkt));

	k_sem_reset(&xfer_done);
	int ret = uhc_ep_enqueue(uhc_dev, &xfer);
	if (ret) {
		LOG_ERR("setup enqueue failed: %d", ret);
		return ret;
	}
	if (k_sem_take(&xfer_done, K_MSEC(1500))) {
		LOG_WRN("setup timed out");
		(void)uhc_ep_dequeue(uhc_dev, &xfer);
		return -ETIMEDOUT;
	}
	return xfer.err;
}

static int get_descriptor(uint8_t addr, uint16_t mps, uint8_t type,
			  uint8_t index, uint16_t langid, uint16_t length,
			  struct net_buf *buf)
{
	struct usb_setup_packet setup = {
		.RequestType = {
			.recipient = USB_REQTYPE_RECIPIENT_DEVICE,
			.type = USB_REQTYPE_TYPE_STANDARD,
			.direction = USB_REQTYPE_DIR_TO_HOST,
		},
		.bRequest = USB_SREQ_GET_DESCRIPTOR,
		.wValue = sys_cpu_to_le16(((uint16_t)type << 8) | index),
		.wIndex = sys_cpu_to_le16(langid),
		.wLength = sys_cpu_to_le16(length),
	};
	return issue_setup(addr, mps, &setup, buf);
}

static int set_address(uint16_t mps, uint8_t new_addr)
{
	struct usb_setup_packet setup = {
		.RequestType = {
			.recipient = USB_REQTYPE_RECIPIENT_DEVICE,
			.type = USB_REQTYPE_TYPE_STANDARD,
			.direction = USB_REQTYPE_DIR_TO_DEVICE,
		},
		.bRequest = USB_SREQ_SET_ADDRESS,
		.wValue = sys_cpu_to_le16(new_addr),
		.wIndex = 0,
		.wLength = 0,
	};
	return issue_setup(0, mps, &setup, NULL);
}

/* ===== String-descriptor decoding ================================== */

/* Decode a USB string descriptor (UTF-16LE payload) into ASCII for log
 * output. Non-ASCII codepoints are replaced with '?'. Output is always
 * NUL-terminated. */
static void decode_string_desc(const uint8_t *desc, size_t desc_len,
			       char *out, size_t out_size)
{
	if (out_size == 0) {
		return;
	}
	out[0] = '\0';
	if (desc_len < 2 || desc[1] != USB_DESC_STRING) {
		return;
	}
	size_t bytes = desc[0];
	if (bytes > desc_len) {
		bytes = desc_len;
	}
	if (bytes < 2) {
		return;
	}
	size_t chars = (bytes - 2) / 2;
	size_t out_i = 0;
	for (size_t i = 0; i < chars && out_i + 1 < out_size; i++) {
		uint16_t cp = sys_get_le16(&desc[2 + i * 2]);
		out[out_i++] = (cp < 0x80 && cp >= 0x20) ? (char)cp : '?';
	}
	out[out_i] = '\0';
}

static int fetch_string(uint16_t mps, uint8_t index, uint16_t langid,
			char *out, size_t out_size)
{
	if (out_size > 0) {
		out[0] = '\0';
	}
	if (index == 0) {
		return 0;
	}

	struct net_buf *buf = net_buf_alloc(&desc_pool, K_MSEC(100));
	if (buf == NULL) {
		return -ENOMEM;
	}

	int ret = get_descriptor(ENUM_ADDR, mps, USB_DESC_STRING, index, langid,
				 DESC_BUF_SIZE, buf);
	if (ret == 0 && buf->len >= 2) {
		decode_string_desc(buf->data, buf->len, out, out_size);
	}
	net_buf_unref(buf);
	return ret;
}

/* ===== Pretty-print device descriptor ============================== */

static void print_device_descriptor(const struct usb_device_descriptor *d,
				    const char *manufacturer,
				    const char *product,
				    const char *serial)
{
	LOG_INF("Device: VID=0x%04x PID=0x%04x", d->idVendor, d->idProduct);
	LOG_INF("  bLength             %u", d->bLength);
	LOG_INF("  bDescriptorType     %u", d->bDescriptorType);
	LOG_INF("  bcdUSB              %04x", d->bcdUSB);
	LOG_INF("  bDeviceClass        %u", d->bDeviceClass);
	LOG_INF("  bDeviceSubClass     %u", d->bDeviceSubClass);
	LOG_INF("  bDeviceProtocol     %u", d->bDeviceProtocol);
	LOG_INF("  bMaxPacketSize0     %u", d->bMaxPacketSize0);
	LOG_INF("  idVendor            0x%04x", d->idVendor);
	LOG_INF("  idProduct           0x%04x", d->idProduct);
	LOG_INF("  bcdDevice           %04x", d->bcdDevice);
	LOG_INF("  iManufacturer       %u  %s", d->iManufacturer, manufacturer);
	LOG_INF("  iProduct            %u  %s", d->iProduct, product);
	LOG_INF("  iSerialNumber       %u  %s", d->iSerialNumber, serial);
	LOG_INF("  bNumConfigurations  %u", d->bNumConfigurations);
}

/* ===== Enumeration ================================================== */

static int enumerate_and_dump(void)
{
	int ret;

	LOG_INF("Issuing bus reset...");
	ret = uhc_bus_reset(uhc_dev);
	if (ret) {
		LOG_ERR("uhc_bus_reset failed: %d", ret);
		return ret;
	}
	/* USB §9.2.6.3: at least 10ms after reset before talking. */
	k_msleep(50);

	struct net_buf *buf = net_buf_alloc(&desc_pool, K_MSEC(100));
	if (buf == NULL) {
		LOG_ERR("descriptor net_buf alloc failed");
		return -ENOMEM;
	}

	/* Step 1: short device-descriptor fetch at addr=0, MPS=8. We only
	 * need bMaxPacketSize0 here so the next transactions can split data
	 * stages correctly. */
	ret = get_descriptor(0, DEFAULT_MPS0, USB_DESC_DEVICE, 0, 0, 8, buf);
	if (ret || buf->len < 8) {
		LOG_ERR("GET_DESCRIPTOR(DEVICE,8) at addr=0 failed: ret=%d len=%u",
			ret, buf->len);
		net_buf_unref(buf);
		return ret ? ret : -EIO;
	}
	uint16_t mps0 = buf->data[7];
	if (mps0 == 0) {
		mps0 = DEFAULT_MPS0;
	}
	LOG_INF("addr=0: bMaxPacketSize0=%u", mps0);

	/* Step 2: SET_ADDRESS(1). */
	ret = set_address(mps0, ENUM_ADDR);
	if (ret) {
		LOG_ERR("SET_ADDRESS(%u) failed: %d", ENUM_ADDR, ret);
		net_buf_unref(buf);
		return ret;
	}
	/* USB §9.2.6.3: 2ms recovery between SET_ADDRESS STATUS and the next
	 * request to the new address. */
	k_msleep(2);

	/* Step 3: full device-descriptor fetch at the assigned address. */
	net_buf_reset(buf);
	ret = get_descriptor(ENUM_ADDR, mps0, USB_DESC_DEVICE, 0, 0,
			     sizeof(struct usb_device_descriptor), buf);
	if (ret || buf->len < sizeof(struct usb_device_descriptor)) {
		LOG_ERR("GET_DESCRIPTOR(DEVICE,18) at addr=%u failed: ret=%d len=%u",
			ENUM_ADDR, ret, buf->len);
		net_buf_unref(buf);
		return ret ? ret : -EIO;
	}

	struct usb_device_descriptor desc;
	memcpy(&desc, buf->data, sizeof(desc));
	desc.bcdUSB = sys_le16_to_cpu(desc.bcdUSB);
	desc.idVendor = sys_le16_to_cpu(desc.idVendor);
	desc.idProduct = sys_le16_to_cpu(desc.idProduct);
	desc.bcdDevice = sys_le16_to_cpu(desc.bcdDevice);
	net_buf_unref(buf);

	/* Step 4: fetch langid table (string descriptor 0). The first u16
	 * past the header is the preferred LANGID; default to 0x0409 (en-US)
	 * if the device has no string descriptors. */
	uint16_t langid = 0x0409;
	struct net_buf *lbuf = net_buf_alloc(&desc_pool, K_MSEC(100));
	if (lbuf != NULL) {
		ret = get_descriptor(ENUM_ADDR, mps0, USB_DESC_STRING, 0, 0, 4,
				     lbuf);
		if (ret == 0 && lbuf->len >= 4) {
			langid = sys_get_le16(&lbuf->data[2]);
		}
		net_buf_unref(lbuf);
	}

	/* Step 5: per-index strings. Failures degrade to "<n/a>" rather than
	 * aborting — many devices skip serial numbers, and we still want to
	 * print whatever we got. */
	char manufacturer[64], product[64], serial[64];
	(void)fetch_string(mps0, desc.iManufacturer, langid, manufacturer,
			   sizeof(manufacturer));
	(void)fetch_string(mps0, desc.iProduct, langid, product,
			   sizeof(product));
	(void)fetch_string(mps0, desc.iSerialNumber, langid, serial,
			   sizeof(serial));
	if (manufacturer[0] == '\0') strcpy(manufacturer, "<n/a>");
	if (product[0] == '\0') strcpy(product, "<n/a>");
	if (serial[0] == '\0') strcpy(serial, "<n/a>");

	print_device_descriptor(&desc, manufacturer, product, serial);
	return 0;
}

/* ===== Main ========================================================= */

int main(void)
{
	int ret;

	printk("\n*** MAIN_START ***\n");
	wait_for_console();

	k_sem_init(&connect_sem, 0, 1);
	k_sem_init(&xfer_done, 0, 1);

	LOG_INF("Pico-PIO-USB host descriptor-dump sample starting");

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

	ret = uhc_enable(uhc_dev);
	if (ret) {
		LOG_ERR("uhc_enable failed: %d", ret);
		return ret;
	}
	LOG_INF("UHC enabled — plug a USB-FS device into the Link USB-C port");

	while (1) {
		k_sem_take(&connect_sem, K_FOREVER);
		if (!atomic_get(&connected)) {
			continue;
		}
		ret = enumerate_and_dump();
		if (ret) {
			LOG_WRN("Enumeration failed (%d) — waiting for next plug", ret);
		}
		/* Wait for the device to come back (replug or successful next
		 * connect event) before doing it again. */
	}
	return 0;
}
