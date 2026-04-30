/*
 * Copyright (c) 2026 The Cosmos-Keyboards Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Pico-PIO-USB device standalone sample. Brings up udc_pio_usb on a
 * Lemon Wired and registers a CDC ACM device on top of it via Zephyr's
 * USBD-next stack.
 *
 * Plug the Lemon's Link USB-C port into a host computer (or into a
 * second Lemon flashed with samples/pio_usb_host/). The host should
 * see this device enumerate as a CDC ACM serial port — open it and
 * type; characters are echoed back.
 *
 * Native USB-C remains available for the Zephyr console / log output
 * (CONFIG_LOG_PRINTK), kept on stdio.
 */

#include <stdio.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/usb/usbd.h>

LOG_MODULE_REGISTER(pio_usb_device_sample, LOG_LEVEL_INF);

/* USBD device + descriptor declarations (USBD-next style — same as
 * Zephyr's upstream samples/subsys/usb/cdc_acm but pointing at our
 * Pico-PIO-USB UDC node instead of the native zephyr_udc0). */

USBD_CONFIGURATION_DEFINE(config_1, USB_SCD_SELF_POWERED, 200);
USBD_DESC_LANG_DEFINE(sample_lang);
USBD_DESC_MANUFACTURER_DEFINE(sample_mfr, "Cosmos");
USBD_DESC_PRODUCT_DEFINE(sample_product, "Lemon UDC PIO-USB Test");
USBD_DESC_SERIAL_NUMBER_DEFINE(sample_sn, "0123456789AB");

USBD_DEVICE_DEFINE(sample_usbd,
		   DEVICE_DT_GET(DT_NODELABEL(pio_usb_device)),
		   /* VID/PID — placeholder; replace with your own when
		    * shipping a real product. */
		   0x2fe3, 0x0003);

#define ECHO_RING_BUF_SIZE 1024
static uint8_t ring_storage[ECHO_RING_BUF_SIZE];
static struct ring_buf ringbuf;

static const struct device *const cdc_dev =
	DEVICE_DT_GET(DT_NODELABEL(cdc_acm_uart0));

static void cdc_irq_handler(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);

	while (uart_irq_update(dev) && uart_irq_is_pending(dev)) {
		if (uart_irq_rx_ready(dev)) {
			uint8_t buf[64];
			size_t len = MIN(ring_buf_space_get(&ringbuf), sizeof(buf));
			int n = uart_fifo_read(dev, buf, len);
			if (n > 0) {
				(void)ring_buf_put(&ringbuf, buf, n);
				uart_irq_tx_enable(dev);
			}
		}
		if (uart_irq_tx_ready(dev)) {
			uint8_t buf[64];
			int n = ring_buf_get(&ringbuf, buf, sizeof(buf));
			if (n <= 0) {
				uart_irq_tx_disable(dev);
				continue;
			}
			(void)uart_fifo_fill(dev, buf, n);
		}
	}
}

static int enable_usbd(void)
{
	int ret;

	ret = usbd_add_descriptor(&sample_usbd, &sample_lang);
	if (ret) return ret;
	ret = usbd_add_descriptor(&sample_usbd, &sample_mfr);
	if (ret) return ret;
	ret = usbd_add_descriptor(&sample_usbd, &sample_product);
	if (ret) return ret;
	ret = usbd_add_descriptor(&sample_usbd, &sample_sn);
	if (ret) return ret;
	ret = usbd_add_configuration(&sample_usbd, &config_1);
	if (ret) return ret;

	ret = usbd_register_class(&sample_usbd, "cdc_acm_0", 1);
	if (ret) return ret;

	ret = usbd_init(&sample_usbd);
	if (ret) return ret;
	return usbd_enable(&sample_usbd);
}

int main(void)
{
	int ret;

	LOG_INF("Pico-PIO-USB device sample starting");

	ring_buf_init(&ringbuf, sizeof(ring_storage), ring_storage);

	if (!device_is_ready(cdc_dev)) {
		LOG_ERR("CDC ACM device not ready");
		return -ENODEV;
	}

	ret = enable_usbd();
	if (ret) {
		LOG_ERR("USBD enable failed: %d", ret);
		return ret;
	}

	uart_irq_callback_set(cdc_dev, cdc_irq_handler);
	uart_irq_rx_enable(cdc_dev);

	LOG_INF("UDC enabled — Link USB-C port now exposes a CDC ACM device");

	while (1) {
		k_msleep(1000);
	}
	return 0;
}
