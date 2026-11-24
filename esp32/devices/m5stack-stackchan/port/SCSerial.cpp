/*
 * M5Stack's vendor/m5stack/hal/drivers/FTServo_Arduino/src/SCSerial.cpp
 * (FTServo_Arduino, MIT: vendor/m5stack/hal/drivers/FTServo_Arduino/LICENSE),
 * built in its place with the two readSCS functions replaced (marked
 * "Impo:"); the rest is M5Stack's as it is. vendor/README.md says why.
 *
 * SCSerial.cpp
 * 飞特串行舵机硬件接口层程序
 * 日期: 2024.4.2
 * 作者:
 */

#include "hal/drivers/FTServo_Arduino/src/SCSerial.h"

#include <atomic>

#include "hal_impo.h"

#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "FT_SCSerial";

SCSerial::SCSerial()
{
	IOTimeOut = 10;
	uart_num = UART_NUM_MAX;
}

SCSerial::SCSerial(u8 End):SCS(End)
{
	IOTimeOut = 10;
	uart_num = UART_NUM_MAX;
}

SCSerial::SCSerial(u8 End, u8 Level):SCS(End, Level)
{
	IOTimeOut = 10;
	uart_num = UART_NUM_MAX;
}

bool SCSerial::begin(uart_port_t uart_num, int baud_rate, int tx_pin, int rx_pin, int buf_size)
{
	this->uart_num = uart_num;

	uart_config_t uart_config = {
		.baud_rate = baud_rate,
		.data_bits = UART_DATA_8_BITS,
		.parity = UART_PARITY_DISABLE,
		.stop_bits = UART_STOP_BITS_1,
		.flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
		.rx_flow_ctrl_thresh = 0,
		.source_clk = UART_SCLK_DEFAULT,
	};

	esp_err_t ret = uart_driver_install(uart_num, buf_size, buf_size, 0, NULL, 0);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "Failed to install UART driver: %s", esp_err_to_name(ret));
		return false;
	}

	ret = uart_param_config(uart_num, &uart_config);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "Failed to configure UART: %s", esp_err_to_name(ret));
		uart_driver_delete(uart_num);
		this->uart_num = UART_NUM_MAX;
		return false;
	}

	ret = uart_set_pin(uart_num, tx_pin, rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "Failed to set UART pins: %s", esp_err_to_name(ret));
		uart_driver_delete(uart_num);
		this->uart_num = UART_NUM_MAX;
		return false;
	}

	return true;
}

void SCSerial::end()
{
	if (uart_num < UART_NUM_MAX) {
		uart_driver_delete(uart_num);
		uart_num = UART_NUM_MAX;
	}
}

/* Impo: the two readSCS below replace M5Stack's, which read one byte per pass
 * with a 1 ms delay after each: a reply cost about 1 ms per byte, a write's
 * acknowledgement about 6 ms and a read about 9 ms, so a 20 ms motion tick
 * took 15 to 30 ms and replies longer than the timeout came back short (-1).
 * These take what has arrived in one call and block in the UART driver for
 * the rest, with the same timeouts. */
static int read_into(uart_port_t uart_num, unsigned char *nDat, int size, int want, TickType_t ticks)
{
	unsigned char scratch[32];
	if (!nDat && want > (int)sizeof(scratch)) {
		want = sizeof(scratch);
	}
	return uart_read_bytes(uart_num, nDat ? nDat + size : scratch, want, ticks);
}

/* Replies that did not arrive whole: a reading taken while this stays the
 * same came from the servo itself (hal_impo.h). */
static std::atomic<unsigned> s_short_reads{0};

unsigned hal_impo_servo_short_reads(void)
{
	return s_short_reads.load();
}

/* Up to nLen bytes within TimeOut ms of the call. */
int SCSerial::readSCS(unsigned char *nDat, int nLen, unsigned long TimeOut)
{
	if (uart_num >= UART_NUM_MAX) {
		return 0;
	}

	int size = 0;
	int64_t end = esp_timer_get_time() + static_cast<int64_t>(TimeOut) * 1000;
	while (size < nLen) {
		int64_t left_us = end - esp_timer_get_time();
		if (left_us <= 0) {
			break;
		}
		int got = read_into(uart_num, nDat, size, nLen - size, pdMS_TO_TICKS(left_us / 1000) + 1);
		if (got <= 0) {
			break;
		}
		size += got;
	}
	if (size < nLen) {
		s_short_reads++;
	}
	return size;
}

/* Up to nLen bytes, giving up when none arrives for IOTimeOut ms. */
int SCSerial::readSCS(unsigned char *nDat, int nLen)
{
	if (uart_num >= UART_NUM_MAX) {
		return 0;
	}

	int size = 0;
	while (size < nLen) {
		int got = read_into(uart_num, nDat, size, nLen - size, pdMS_TO_TICKS(IOTimeOut) + 1);
		if (got <= 0) {
			break;
		}
		size += got;
	}
	if (size < nLen) {
		s_short_reads++;
	}
	return size;
}

int SCSerial::writeSCS(unsigned char *nDat, int nLen)
{
	if (uart_num >= UART_NUM_MAX || nDat == NULL) {
		return 0;
	}

	return uart_write_bytes(uart_num, reinterpret_cast<const char *>(nDat), nLen);
}

int SCSerial::writeSCS(unsigned char bDat)
{
	if (uart_num >= UART_NUM_MAX) {
		return 0;
	}

	return uart_write_bytes(uart_num, reinterpret_cast<const char *>(&bDat), 1);
}

void SCSerial::rFlushSCS()
{
	if (uart_num >= UART_NUM_MAX) {
		return;
	}

	uart_flush_input(uart_num);
}

void SCSerial::wFlushSCS()
{
	if (uart_num >= UART_NUM_MAX) {
		return;
	}

	uart_wait_tx_done(uart_num, pdMS_TO_TICKS(100));
}