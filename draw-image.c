/*
 ============================================================================
 Name        : draw-image.c
 Author      : 
 Version     :
 Copyright   : Your copyright notice
 Description : Hello World in C, Ansi-style
 ============================================================================
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/ioctl.h>

#include <gpiod.h>
#include <linux/ioctl.h>
#include <linux/types.h>
#include <linux/spi/spidev.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"

#define MIN(a, b)					((a) > (b) ? (b) : (a))

#define ARRAY_SIZEOF(x)				(sizeof(x) / (sizeof(x[0])))

#define RGB(r, g, b)				((((r) >> 3) << 11) | (((g) >> 2) << 5) | (((b) >> 3) << 0))

#define LINE_RESET					16
#define LINE_DC						19
#define SPI_RD_SPEED				1000000
#define SPI_WR_SPEED				24000000

static struct gpiod_line_request* _gpiod_request_output_line(const char *path, uint32_t offset, bool active_low, enum gpiod_line_value value, const char *consumer);
static int _spi_open(const char *dev_path);
static int _display_do_initialize(int spi_fd, struct gpiod_line_request *io_reset, struct gpiod_line_request *io_dc);
static int _spi_send_cmd(int fd, struct gpiod_line_request *io_dc, uint8_t cmd, uint8_t *data, size_t data_len);
static int _spi_read_cmd(int fd, struct gpiod_line_request *io_dc, uint8_t cmd, uint8_t *data, size_t data_len);
static int _spi_transfer8(int fd, uint8_t const *tx, uint8_t const *rx, size_t len);
static int _spi_enter_data_mode(int spi_fd, struct gpiod_line_request *io_dc);

static int _display_do_display_test(int spi_fd, int pause_on_end);
static int _display_do_display_image(int spi_fd, const char *file_path);
static int _spi_transfer16(int fd, uint16_t const *tx, uint16_t const *rx, size_t len);

static uint32_t spi_speed = SPI_RD_SPEED;
static uint32_t spi_pagesize = 4096;

int main(int argc, char **argv)
{
	// parse the command line options
	int do_initialize = 0;
	int do_test = 0;

	int opt = 0;
	while ((opt = getopt(argc, argv, "it")) != -1)
	{
		switch (opt)
		{
		case 'i':
			do_initialize = 1;
			break;

		case 't':
			do_test = 1;
			break;

		default:
			printf("usage: %s [-i] [file-name]\r\n", argv[0]);
			return -1;
		}
	}

	int has_file = (argc > optind);

	// get the GPIO chip and lines
	printf("open the displays RESET and D/CX GPIO lines\r\n");
	struct gpiod_line_request* io_reset = _gpiod_request_output_line("/dev/gpiochip0", LINE_RESET, true, GPIOD_LINE_VALUE_INACTIVE, "display_reset");
	struct gpiod_line_request* io_dc = _gpiod_request_output_line("/dev/gpiochip0", LINE_DC, false, GPIOD_LINE_VALUE_ACTIVE, "display_dc");
	if (! io_reset || ! io_dc)
		return -1;

	// open the SPI bus
	printf("open the displays SPI bus\r\n");
	int spi_fd = _spi_open("/dev/spidev0.0");
	if (spi_fd < 0)
		return -1;

	if (do_initialize)
		if (_display_do_initialize(spi_fd, io_reset, io_dc) < 0)
			return -1;

	if (_spi_enter_data_mode(spi_fd, io_dc) < 0)
		return -1;

	if (do_test)
		if (_display_do_display_test(spi_fd, has_file) < 0)
			return -1;

	if (has_file)
		if (_display_do_display_image(spi_fd, argv[argc - 1]) < 0)
			return -1;

	// close the SPI bus
	close(spi_fd);

	// close the GPIO lines and chip
	gpiod_line_request_release(io_reset);
	gpiod_line_request_release(io_dc);

	return 0;
}

static int _spi_open(const char *dev_path)
{
	char ps_buffer[128];
	memset(ps_buffer, 0, sizeof(ps_buffer));

	// get the SPI page size
	int ps_fd = open("/sys/module/spidev/parameters/bufsiz", O_RDONLY);
	if (ps_fd < 0)
	{
		perror("open(bufsize)");
		return -1;
	}

	int size = read(ps_fd, ps_buffer, sizeof(ps_buffer));
	if (size < 0)
	{
		perror("read(bufsize)");
		return -1;
	}

	if (close(ps_fd) < 0)
	{
		perror("close(bufsize)");
		return -1;
	}

	char *end;
	spi_pagesize = strtol(ps_buffer, &end, 10);
	if (spi_pagesize < (320 * 240 * sizeof(uint16_t)))
	{
		printf("spidev.bufsiz is not optimized for single transfer, using %d transfers for screen update\r\n",
				(320 * 240 * sizeof(uint16_t) + (spi_pagesize - 1)) / spi_pagesize);
	}
	else
		printf("spidev.bufsiz is optimized for single transfer\r\n");

	// open the SPI bus
	int spi_fd = open(dev_path, O_RDWR);

	uint32_t speed = SPI_WR_SPEED;
	if (ioctl(spi_fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0)
	{
		perror("SPI_IOC_WR_MAX_SPEED_HZ");
		return -1;
	}

	if (ioctl(spi_fd, SPI_IOC_RD_MAX_SPEED_HZ, &speed) < 0)
	{
		perror("SPI_IOC_RD_MAX_SPEED_HZ");
		return -1;
	}

	uint8_t bits = 8;
	if (ioctl(spi_fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0)
	{
		perror("SPI_IOC_WR_BITS_PER_WORD(8)");
		return -1;
	}

	if (ioctl(spi_fd, SPI_IOC_RD_BITS_PER_WORD, &bits)< 0)
	{
		perror("SPI_IOC_RD_BITS_PER_WORD(8)");
		return -1;
	}

	return spi_fd;
}

static struct gpiod_line_request* _gpiod_request_output_line(const char *path, uint32_t offset, bool active_low, enum gpiod_line_value value, const char *consumer)
{
	struct gpiod_request_config *req_cfg = NULL;
	int ret;

	struct gpiod_chip *chip = gpiod_chip_open(path);
	if (!chip)
		return NULL;

	struct gpiod_line_settings *settings = gpiod_line_settings_new();
	if (!settings)
		goto close_chip;

	gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_OUTPUT);
	gpiod_line_settings_set_output_value(settings, value);
	gpiod_line_settings_set_active_low(settings, active_low);

	struct gpiod_line_config *line_cfg = gpiod_line_config_new();
	if (!line_cfg)
		goto free_settings;

	ret = gpiod_line_config_add_line_settings(line_cfg, &offset, 1, settings);
	if (ret)
		goto free_line_config;

	if (consumer) {
		req_cfg = gpiod_request_config_new();
		if (!req_cfg)
			goto free_line_config;

		gpiod_request_config_set_consumer(req_cfg, consumer);
	}

	struct gpiod_line_request *request = gpiod_chip_request_lines(chip, req_cfg, line_cfg);

	gpiod_request_config_free(req_cfg);

free_line_config:
	gpiod_line_config_free(line_cfg);

free_settings:
	gpiod_line_settings_free(settings);

close_chip:
	gpiod_chip_close(chip);

	return request;
}

static int _display_do_initialize(int spi_fd, struct gpiod_line_request *io_reset, struct gpiod_line_request *io_dc)
{
	printf("do display initialization\r\n");

	// configure the display

	// set display off
	if (_spi_send_cmd(spi_fd, io_dc, 0x28, NULL, 0) < 0)
		return -1;

	// toggle the display reset line
	if (gpiod_line_request_set_value(io_reset, LINE_RESET, GPIOD_LINE_VALUE_INACTIVE) < 0)
		return -1;

	usleep(1000);

	if (gpiod_line_request_set_value(io_reset, LINE_RESET, GPIOD_LINE_VALUE_ACTIVE) < 0)
		return -1;

	usleep(100000);

	if (gpiod_line_request_set_value(io_reset, LINE_RESET, GPIOD_LINE_VALUE_INACTIVE) < 0)
		return -1;

	usleep(250000);

	// make the display wakeup
	if (_spi_send_cmd(spi_fd, io_dc, 0x11, NULL, 0) < 0)
		return -1;

	usleep(130000);

	if (_spi_send_cmd(spi_fd, io_dc, 0x11, NULL, 0) < 0)
		return -1;

	usleep(130000);

	// read the display ID
	{
		uint8_t data[4] = { 0, 0, 0, 0 };
		if (_spi_read_cmd(spi_fd, io_dc, 0x04, data, sizeof(data)) < 0)
			return -1;

		printf("RDID: 0x%02x 0x%02x 0x%02x 0x%02x\r\n", data[0], data[1], data[2], data[3]);
	}

	// set the display orientation
	{
		//uint8_t data = 0x60; // rotate 180 degree
		uint8_t data = 0xa0;
		if (_spi_send_cmd(spi_fd, io_dc, 0x36, &data, sizeof(data)) < 0)
			return -1;
	}

	// set the color scheme
	{
		uint8_t data = 0x55;
		if (_spi_send_cmd(spi_fd, io_dc, 0x3a, &data, sizeof(data)) < 0)
			return -1;
	}

	// set the display porch
	{
		uint8_t data[5] = { 0x0c, 0x0c, 0x00, 0x33, 0x33 };
		if (_spi_send_cmd(spi_fd, io_dc, 0xb2, data, sizeof(data)) < 0)
			return -1;
	}

	// set the gate control
	{
		uint8_t data = 0x50;
		if (_spi_send_cmd(spi_fd, io_dc, 0xb7, &data, sizeof(data)) < 0)
			return -1;
	}

	// set the VCOMs
	{
		uint8_t data = 0x2b;
		if (_spi_send_cmd(spi_fd, io_dc, 0xbb, &data, sizeof(data)) < 0)
			return -1;
	}

	// set the LCM control
	{
		uint8_t data = 0x2c;
		if (_spi_send_cmd(spi_fd, io_dc, 0xc0, &data, sizeof(data)) < 0)
			return -1;
	}

	// enable the VDV and VRH commands
	{
		uint8_t data = 0x01;
		if (_spi_send_cmd(spi_fd, io_dc, 0xc2, &data, sizeof(data)) < 0)
			return -1;
	}

	// set the VRH
	{
		uint8_t data = 0x0b;
		if (_spi_send_cmd(spi_fd, io_dc, 0xc3, &data, sizeof(data)) < 0)
			return -1;
	}

	// set the VDV
	{
		uint8_t data = 0x20;
		if (_spi_send_cmd(spi_fd, io_dc, 0xc4, &data, sizeof(data)) < 0)
			return -1;
	}

	// set the frame rate control in normal mode
	{
		uint8_t data = 0x0f;
		if (_spi_send_cmd(spi_fd, io_dc, 0xc6, &data, sizeof(data)) < 0)
			return -1;
	}

	// set the power control
	{
		uint8_t data[2] = { 0xa4, 0xa1 };
		if (_spi_send_cmd(spi_fd, io_dc, 0xd0, data, sizeof(data)) < 0)
			return -1;
	}

	// set the display gamma settings
	{
		uint8_t data[14] = { 0xd0, 0x00, 0x02, 0x07, 0x0b, 0x1a, 0x31, 0x54, 0x40, 0x29, 0x12, 0x12, 0x12, 0x17 };
		if (_spi_send_cmd(spi_fd, io_dc, 0xe0, data, sizeof(data)) < 0)
			return -1;
	}

	// ... continue setting the display gamma
	{
		uint8_t data[14] = { 0xd0, 0x00, 0x02, 0x07, 0x05, 0x25, 0x2d, 0x44, 0x45, 0x1c, 0x18, 0x16, 0x1c, 0x1d };
		if (_spi_send_cmd(spi_fd, io_dc, 0xe1, data, sizeof(data)) < 0)
			return -1;
	}

	// set the column address
	{
		uint8_t data[4] =
		{
				0x00, 0x00,
				(320 - 1) >> 8, (320 - 1) & 0xff
		};
		if (_spi_send_cmd(spi_fd, io_dc, 0x2a, data, sizeof(data)) < 0)
			return -1;
	}

	// set the row address
	{
		uint8_t data[4] =
		{
				0x00, 0x00,
				(240 - 1) >> 8, (240 - 1) & 0xff
		};
		if (_spi_send_cmd(spi_fd, io_dc, 0x2b, data, sizeof(data)) < 0)
			return -1;
	}

	// turn the display on
	if (_spi_send_cmd(spi_fd, io_dc, 0x29, NULL, 0) < 0)
		return -1;

	return 0;
}

static int _spi_send_cmd(int fd, struct gpiod_line_request *io_dc, uint8_t cmd, uint8_t *data, size_t data_len)
{
	gpiod_line_request_set_value(io_dc, LINE_DC, GPIOD_LINE_VALUE_INACTIVE);

	uint8_t cmd_dummy;
	if (_spi_transfer8(fd, &cmd, &cmd_dummy, 1) < 0)
		return -1;

	if (data && data_len)
	{
		gpiod_line_request_set_value(io_dc, LINE_DC, GPIOD_LINE_VALUE_ACTIVE);

		uint8_t data_dummy[data_len];
		if (_spi_transfer8(fd, data, data_dummy, data_len) < 0)
			return -1;
	}

	return 0;
}

static int _spi_read_cmd(int fd, struct gpiod_line_request *io_dc, uint8_t cmd, uint8_t *data, size_t data_len)
{
	gpiod_line_request_set_value(io_dc, LINE_DC, GPIOD_LINE_VALUE_INACTIVE);

	uint8_t cmd_dummy;
	if (_spi_transfer8(fd, &cmd, &cmd_dummy, 1) < 0)
		return -1;

	if (data && data_len)
	{
		gpiod_line_request_set_value(io_dc, LINE_DC, GPIOD_LINE_VALUE_ACTIVE);

		if (_spi_transfer8(fd, data, data, data_len) < 0)
			return -1;
	}

	return 0;
}

static int _spi_transfer8(int fd, uint8_t const *tx, uint8_t const *rx, size_t len)
{
	struct spi_ioc_transfer tr = {
		.tx_buf = (unsigned long)tx,
		.rx_buf = (unsigned long)rx,
		.len = len,
		.delay_usecs = 0,
		.word_delay_usecs = 0,
		.speed_hz = spi_speed,
		.bits_per_word = 8,
		.tx_nbits = 8,
		.rx_nbits = 8,
	};

	int ret = ioctl(fd, SPI_IOC_MESSAGE(1), &tr);
	if (ret < 0)
	{
		perror("_spi_transfer8");
		return -1;
	}

	return 0;
}

static int _spi_enter_data_mode(int spi_fd, struct gpiod_line_request *io_dc)
{
	// send write memory command
	if (_spi_send_cmd(spi_fd, io_dc, 0x2c, NULL, 0) < 0)
		return -1;

	// switch to high speed SPI clock
	spi_speed = SPI_WR_SPEED;

	// enter data mode
	if (gpiod_line_request_set_value(io_dc, LINE_DC, GPIOD_LINE_VALUE_ACTIVE) < 0)
		return -1;

	// switch SPI to 16 bit mode
	uint8_t bits = 16;
	if (ioctl(spi_fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0)
	{
		perror("SPI_IOC_WR_BITS_PER_WORD(16)");
		return -1;
	}

	if (ioctl(spi_fd, SPI_IOC_RD_BITS_PER_WORD, &bits)< 0)
	{
		perror("SPI_IOC_RD_BITS_PER_WORD(16)");
		return -1;
	}

	return 0;
}

static int _display_do_display_test(int spi_fd, int pause_on_end)
{
	char line_buffer[1024];
	uint16_t fb[240 * 320];

	// black
	for (int idx = 0; idx < ARRAY_SIZEOF(fb); ++idx)
		fb[idx] = 0x0000;

	if (_spi_transfer16(spi_fd, fb, fb, ARRAY_SIZEOF(fb)) < 0)
		return -1;

	printf("(black) press enter to continue...\r\n");
	fgets(line_buffer, sizeof(line_buffer), stdin);

	// white
	for (int idx = 0; idx < ARRAY_SIZEOF(fb); ++idx)
		fb[idx] = 0xffff;

	if (_spi_transfer16(spi_fd, fb, fb, ARRAY_SIZEOF(fb)) < 0)
		return -1;

	printf("(white) press enter to continue...\r\n");
	fgets(line_buffer, sizeof(line_buffer), stdin);

	// red
	for (int idx = 0; idx < ARRAY_SIZEOF(fb); ++idx)
		fb[idx] = 0xf800;

	if (_spi_transfer16(spi_fd, fb, fb, ARRAY_SIZEOF(fb)) < 0)
		return -1;

	printf("(red) press enter to continue...\r\n");
	fgets(line_buffer, sizeof(line_buffer), stdin);

	// green
	for (int idx = 0; idx < ARRAY_SIZEOF(fb); ++idx)
		fb[idx] = 0x07e0;

	if (_spi_transfer16(spi_fd, fb, fb, ARRAY_SIZEOF(fb)) < 0)
		return -1;

	printf("(green) press enter to continue...\r\n");
	fgets(line_buffer, sizeof(line_buffer), stdin);

	// blue
	for (int idx = 0; idx < ARRAY_SIZEOF(fb); ++idx)
		fb[idx] = 0x003f;

	if (_spi_transfer16(spi_fd, fb, fb, ARRAY_SIZEOF(fb)) < 0)
		return -1;

	printf("(blue) press enter to continue...\r\n");
	fgets(line_buffer, sizeof(line_buffer), stdin);

	// tri-color
	for (int idx = 0; idx < (ARRAY_SIZEOF(fb) / 3); ++idx)
		fb[idx] = 0xf800; // red

	for (int idx = (ARRAY_SIZEOF(fb) / 3); idx < (ARRAY_SIZEOF(fb) / 3 * 2); ++idx)
		fb[idx] = 0x07e0; // green

	for (int idx = (ARRAY_SIZEOF(fb) / 3 * 2); idx < ARRAY_SIZEOF(fb); ++idx)
		fb[idx] = 0x003f; // blue

	if (_spi_transfer16(spi_fd, fb, fb, ARRAY_SIZEOF(fb)) < 0)
		return -1;

	printf("(tri-color) press enter to continue...\r\n");
	if (pause_on_end)
		fgets(line_buffer, sizeof(line_buffer), stdin);

	return 0;
}

static int _display_do_display_image(int spi_fd, const char *file_path)
{
	struct stat fstat;
	if (stat(file_path, &fstat) < 0)
	{
		perror("stat(file)");
		return -1;
	}

	uint8_t raw_buffer[fstat.st_size];
	int file_fd = open(file_path, O_RDONLY);
	if (file_fd < 0)
	{
		perror("open(file)");
		return -1;
	}

	if (read(file_fd, raw_buffer, fstat.st_size) < 0)
	{
		perror("read(file)");
		return -1;
	}

	if (close(file_fd) < 0)
	{
		perror("close(file)");
		return -1;
	}

	int width = 0;
	int height = 0;
	int channels = 0;
	uint8_t *img = stbi_load_from_memory(raw_buffer, fstat.st_size, &width, &height, &channels, 0);

	if (width != 320 || height != 240)
	{
		printf("image must have a size of 320x240\r\n");
		return -1;
	}

	uint16_t fb[320 * 240];
	for (int idx = 0; idx < (width * height); ++idx)
	{
		uint8_t r = img[idx * channels + 0];
		uint8_t g = img[idx * channels + 1];
		uint8_t b = img[idx * channels + 2];

		fb[idx] = RGB(r, g, b);
	}

	stbi_image_free(img);

	if (_spi_transfer16(spi_fd, fb, fb, ARRAY_SIZEOF(fb)) < 0)
		return -1;

	return 0;
}

static int _spi_transfer16(int fd, uint16_t const *tx, uint16_t const *rx, size_t len)
{
	// split the large transfers into page (spi_pagesize bytes) size transfers
	while (len > 0)
	{
		size_t transfer = MIN(len, (spi_pagesize / sizeof(uint16_t)));

		struct spi_ioc_transfer tr = {
			.tx_buf = (unsigned long)tx,
			.rx_buf = (unsigned long)rx,
			.len = transfer * sizeof(uint16_t),
			.delay_usecs = 0,
			.word_delay_usecs = 0,
			.speed_hz = spi_speed,
			.bits_per_word = 16,
			.tx_nbits = 8,
			.rx_nbits = 8,
		};

		int ret = ioctl(fd, SPI_IOC_MESSAGE(1), &tr);
		if (ret < 0)
		{
			perror("_spi_transfer16");
			return -1;
		}

		len -= transfer;
		tx += transfer;
		rx += transfer;
	}

	return 0;
}
