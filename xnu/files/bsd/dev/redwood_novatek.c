/*
 * The touch screen of the Redmi 10 family (selene / selenes / eos): a Novatek NT36672C
 * on SPI0 of the MT6769H / MT6768, as a source of touches for /dev/redwood-touch
 * (sys/redwood_input.h). Only with boot-arg touchscreen=3.
 *
 * Hardware facts:
 *   SPI controller: SPI0 at 0x1100a000 ("mediatek,mt6765-spi", cs 0, up to 9.6 MHz)
 *   GPIO controller: 0x10005000
 *   Pins:
 *     GPIO 1:  IRQ line (active low while the controller has a report waiting)
 *     GPIO 92: Reset line (active low, driven high during operation)
 *     GPIO 25: Chip Select (function 2: SPI0_CS)
 *     GPIO 26: MOSI / Data Out (function 2: SPI0_MO)
 *     GPIO 27: CLK / Clock (function 2: SPI0_CLK)
 *     GPIO 28: MISO / Data In (function 2: SPI0_MI)
 *
 * Protocol:
 *   Novatek NT36672C uses SPI Mode 0 (CPOL=0, CPHA=0).
 *   Point data packet layout (up to 10 fingers, 6 bytes each):
 *     pos = 1 + 6 * finger:
 *       byte 0: [7:3] finger ID (input_id), [2:0] status (1 = down, 2 = move, 0/3 = up)
 *       byte 1: X[11:4]
 *       byte 2: Y[11:4]
 *       byte 3: [7:4] X[3:0], [3:0] Y[3:0]
 *       byte 4: touch width / area
 *       byte 5: touch pressure
 *
 * Panel coordinates: 1080 x 2400 (NT36672C).
 */

#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/redwood_input.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);

#define SPI0_PHYS       0x1100a000ULL
#define SPI0_SIZE       0x1000ULL
#define GPIO_PHYS       0x10005000ULL
#define GPIO_SIZE       0x1000ULL

static vm_offset_t spi, gpio;

static inline uint32_t
rd(vm_offset_t base, uint32_t at)
{
	return *(volatile uint32_t *)(base + at);
}

static inline void
wr(vm_offset_t base, uint32_t at, uint32_t value)
{
	*(volatile uint32_t *)(base + at) = value;
}

/* ---- MT6768 / MT6769 GPIO control ---- */

#define PIN_IRQ         1u              /* GPIO 1: IRQ line (low when report ready) */
#define PIN_RST         92u             /* GPIO 92: Reset line */
#define PIN_CS          25u
#define PIN_MO          26u
#define PIN_CLK         27u
#define PIN_MI          28u
#define PIN_FUNC_SPI    2u

static void
pin_mode(uint32_t pin, uint32_t mode)
{
	uint32_t reg = 0x300u + 0x10u * (pin / 8u);
	uint32_t shift = 4u * (pin % 8u);
	uint32_t val = rd(gpio, reg);

	val = (val & ~(0xfu << shift)) | ((mode & 0xfu) << shift);
	wr(gpio, reg, val);
}

static void
pin_dir_out(uint32_t pin, int level)
{
	uint32_t bank = pin / 32u;
	uint32_t bit = 1u << (pin % 32u);

	wr(gpio, (level ? 0x104u : 0x108u) + 0x10u * bank, bit);  /* DOUT_SET / DOUT_CLR */
	wr(gpio, 0x004u + 0x10u * bank, bit);                     /* DIR_SET */
}

static void
pin_dir_in(uint32_t pin)
{
	uint32_t bank = pin / 32u;
	uint32_t bit = 1u << (pin % 32u);

	wr(gpio, 0x008u + 0x10u * bank, bit);                     /* DIR_CLR */
}

static inline int
pin_get(uint32_t pin)
{
	uint32_t bank = pin / 32u;

	return (rd(gpio, 0x200u + 0x10u * bank) >> (pin % 32u)) & 1u; /* DIN */
}

static void
novatek_pins_init(void)
{
	/* SPI pins: GPIO 25..28 -> Mode 2 (SPI0) */
	for (uint32_t p = PIN_CS; p <= PIN_MI; p++) {
		pin_mode(p, PIN_FUNC_SPI);
	}
	/* Reset: GPIO 92 -> Mode 0 (GPIO), Output High */
	pin_mode(PIN_RST, 0);
	pin_dir_out(PIN_RST, 1);

	/* IRQ: GPIO 1 -> Mode 0 (GPIO), Input */
	pin_mode(PIN_IRQ, 0);
	pin_dir_in(PIN_IRQ);

	__asm__ volatile ("dsb sy");
}

/* ---- MediaTek SPI controller (SPI0) ---- */

#define SPI_CFG0        0x00
#define SPI_CFG1        0x04
#define SPI_TX_DATA     0x10
#define SPI_RX_DATA     0x14
#define SPI_CMD         0x18
#define SPI_STATUS0     0x1c
#define SPI_PAD_SEL     0x24
#define SPI_CFG2        0x28

#define SPI_CMD_ACT     (1u << 0)
#define SPI_CMD_RST     (1u << 2)
#define SPI_CMD_TXMSBF  (1u << 12)
#define SPI_CMD_RXMSBF  (1u << 13)
#define SPI_STATUS0_DONE (1u << 0)
#define SPI_FIFO_BYTES  32

/*
 * Timing in CFG2 (half-periods) for 26 MHz source crystal:
 * 2 ticks = 6.5 MHz (NT36672C device-tree specifies 9.6 MHz max).
 */
#define SPI_TICKS       2

static void
spi_setup(void)
{
	wr(spi, SPI_CMD, SPI_CMD_RST);
	wr(spi, SPI_CMD, 0);
	wr(spi, SPI_CFG2, ((SPI_TICKS - 1) << 0) | ((SPI_TICKS - 1) << 16));
	wr(spi, SPI_CFG0, ((SPI_TICKS - 1) << 0) | ((SPI_TICKS - 1) << 16));
	wr(spi, SPI_PAD_SEL, 0);
}

/*
 * One transfer through the SPI FIFO: up to 32 bytes.
 * Novatek operates in SPI Mode 0 (CPOL=0, CPHA=0: neither bit set).
 */
static bool
spi_transfer_chunk(const uint8_t *tx, uint8_t *rx, uint32_t length)
{
	uint32_t words = (length + 3) / 4;

	if (length == 0 || length > SPI_FIFO_BYTES) {
		return false;
	}
	spi_setup();
	/* Mode 0: MSB first, no CPOL, no CPHA */
	wr(spi, SPI_CMD, SPI_CMD_TXMSBF | SPI_CMD_RXMSBF);
	wr(spi, SPI_CFG1, ((length - 1) << 16) | ((SPI_TICKS - 1) << 0));

	for (uint32_t word = 0; word < words; word++) {
		uint32_t value = 0;

		for (uint32_t byte = 0; byte < 4; byte++) {
			uint32_t at = word * 4 + byte;

			value |= (uint32_t)(tx && at < length ? tx[at] : 0x00) << (8 * byte);
		}
		wr(spi, SPI_TX_DATA, value);
	}
	wr(spi, SPI_CMD, rd(spi, SPI_CMD) | SPI_CMD_ACT);

	for (uint32_t spin = 0; !(rd(spi, SPI_STATUS0) & SPI_STATUS0_DONE); spin++) {
		if (spin > 200000) {
			return false;
		}
	}
	for (uint32_t word = 0; word < words; word++) {
		uint32_t value = rd(spi, SPI_RX_DATA);

		for (uint32_t byte = 0; byte < 4; byte++) {
			uint32_t at = word * 4 + byte;

			if (rx && at < length) {
				rx[at] = (uint8_t)(value >> (8 * byte));
			}
		}
	}
	return true;
}

/* ---- Novatek touch packet read ---- */

#define NVT_READ_ADDR   0x00u           /* Event buffer address */
#define POINT_DATA_LEN  65u             /* 1 header byte + 10 fingers * 6 bytes */
#define MAX_FINGERS     10u

/*
 * Reads the Novatek touch report.
 * Over SPI: Tx contains [0x00 & 0x7F, dummy], Rx returns dummy followed by point_data.
 */
static bool
novatek_read_report(uint8_t *buf, uint32_t max_len)
{
	uint8_t tx[SPI_FIFO_BYTES] = { 0 };
	uint8_t rx[SPI_FIFO_BYTES] = { 0 };

	if (max_len < POINT_DATA_LEN) {
		return false;
	}

	/* Chunk 1: first 32 bytes (includes address 0x00 and dummy) */
	tx[0] = NVT_READ_ADDR & 0x7fu;
	if (!spi_transfer_chunk(tx, rx, 32)) {
		return false;
	}
	/* Byte 0 is response to address, Byte 1 is dummy byte; payload starts at rx[2] */
	memcpy(buf, rx + 2, 30);

	/* Chunk 2: next 32 bytes */
	memset(tx, 0, sizeof(tx));
	if (!spi_transfer_chunk(tx, rx, 32)) {
		return false;
	}
	memcpy(buf + 30, rx, 32);

	/* Chunk 3: remaining bytes */
	if (POINT_DATA_LEN > 62) {
		uint32_t rem = POINT_DATA_LEN - 62;
		if (!spi_transfer_chunk(tx, rx, rem)) {
			return false;
		}
		memcpy(buf + 62, rx, rem);
	}

	return true;
}

/* ---- Touch Coordinate Mapping ---- */

#define PANEL_W         1080u
#define PANEL_H         2400u

static uint32_t fit;                    /* boot-arg fbfit */

/* iPhone X is 1125 x 2436 */
#define PICTURE_H       (PANEL_W * 2436u / 1125u)
#define PICTURE_TOP     ((PANEL_H > PICTURE_H) ? ((PANEL_H - PICTURE_H) / 2u) : 0u)

static uint32_t
picture_x(uint32_t x)
{
	if (x >= PANEL_W) {
		x = PANEL_W - 1;
	}
	return x * 65535u / (PANEL_W - 1);
}

static uint32_t
picture_y(uint32_t y)
{
	if (y >= PANEL_H) {
		y = PANEL_H - 1;
	}
	if (fit == 1 && PICTURE_H > 0 && PANEL_H > PICTURE_H) {
		if (y <= PICTURE_TOP) {
			return 0;
		}
		y -= PICTURE_TOP;
		return y >= PICTURE_H ? 65535u : y * 65535u / (PICTURE_H - 1);
	}
	return y * 65535u / (PANEL_H - 1);
}

static bool down[MAX_FINGERS];
static unsigned reports, touches;
static uint32_t last_x, last_y;

/*
 * Decode Novatek 6-byte records and post to /dev/redwood-touch
 */
static void
process_report(const uint8_t *point_data)
{
	bool seen[MAX_FINGERS] = { false };

	for (uint32_t i = 0; i < MAX_FINGERS; i++) {
		uint32_t pos = 1u + 6u * i;
		uint8_t raw0 = point_data[pos];
		uint32_t finger = raw0 >> 3;
		uint32_t status = raw0 & 0x07u;

		if (finger >= MAX_FINGERS) {
			continue;
		}

		if (status == 0x01 || status == 0x02) { /* Down or Move */
			uint32_t x = ((uint32_t)point_data[pos + 1] << 4) | ((uint32_t)point_data[pos + 3] >> 4);
			uint32_t y = ((uint32_t)point_data[pos + 2] << 4) | ((uint32_t)point_data[pos + 3] & 0x0fu);

			if (x >= PANEL_W) x = PANEL_W - 1;
			if (y >= PANEL_H) y = PANEL_H - 1;

			last_x = x;
			last_y = y;
			seen[finger] = true;
			touches++;

			redwood_touch_post(down[finger] ? REDWOOD_TOUCH_MOVE : REDWOOD_TOUCH_DOWN,
			    finger, picture_x(x), picture_y(y));
			down[finger] = true;
		}
	}

	for (uint32_t finger = 0; finger < MAX_FINGERS; finger++) {
		if (down[finger] && !seen[finger]) {
			redwood_touch_post(REDWOOD_TOUCH_UP, finger, picture_x(last_x), picture_y(last_y));
			down[finger] = false;
		}
	}
	reports++;
}

/* ---- Worker Thread ---- */

static void
novatek_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint8_t buffer[POINT_DATA_LEN + 16];
	uint32_t poll_us = 8000;

	(void)PE_parse_boot_argn("touch_poll_us", &poll_us, sizeof(poll_us));
	(void)PE_parse_boot_argn("fbfit", &fit, sizeof(fit));

	spi = ml_io_map(SPI0_PHYS, SPI0_SIZE);
	gpio = ml_io_map(GPIO_PHYS, GPIO_SIZE);
	if (!spi || !gpio) {
		printf("novatek: cannot map SPI0 or GPIO registers\n");
		return;
	}

	novatek_pins_init();
	spi_setup();
	printf("novatek: NT36672C touch active on SPI0 (0x%llx), polling every %u us\n",
	    SPI0_PHYS, poll_us);

	for (unsigned tick = 1;; tick++) {
		/* IRQ line is active low while touch data is ready */
		while (!pin_get(PIN_IRQ)) {
			if (!novatek_read_report(buffer, sizeof(buffer))) {
				break;
			}
			process_report(buffer);
		}

		if (tick % (60 * (1000000 / poll_us)) == 0) {
			printf("novatek: %u reports, %u touches, last at (%u, %u), IRQ=%s\n",
			    reports, touches, last_x, last_y, pin_get(PIN_IRQ) ? "idle" : "active");
		}
		delay(poll_us);
	}
}

void
redwood_novatek_init(void)
{
	uint32_t which = 0;
	thread_t thread;

	/*
	 * touchscreen=3: Redmi 10 family (selene) Novatek NT36672C.
	 * (touchscreen=2 is Infinix Hot 20 TD4160; touchscreen=1 is Goodix).
	 */
	if (!PE_parse_boot_argn("touchscreen", &which, sizeof(which)) || which != 3) {
		return;
	}
	if (kernel_thread_start(novatek_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		printf("novatek: failed to start touch processing thread\n");
	}
}
