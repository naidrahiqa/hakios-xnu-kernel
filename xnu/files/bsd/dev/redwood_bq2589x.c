/*
 * The charger on the Redmi 10 (selene): a Texas Instruments BQ2589x, compatible "ti,bq25890", on
 * I2C7 (0x11004000) at address 0x6a, the phone's own device tree and Android's log
 * (docs/charger-bq2589x-selene.md). Its interrupt is GPIO 20 and its charge-enable GPIO 152; it is
 * the "primary_chg" Android charges with, at 2.944 A against a 4.45 V limit.
 *
 * Boot-arg bq2589x=1 starts this, and bq2589x=2 says every reading in the log, every 5 s instead of
 * only when something changes. Nothing here writes to the part: the limits are Android's policy
 * (mtk_charger and JEITA), and a read is all a first step needs.
 *
 * The bus is bit-banged, as the other drivers here do their I2C: I2C7's two pins, open drain, the
 * pad pulls left as Android's I2C driver had them (whether the board has its own resistors is what
 * the first reading answers). The pins and the register map are the ones the device tree and TI's
 * SLUSC86 give; a run on the phone is what confirms them.
 *
 * The device tree names no pinctrl group for I2C7, in the base tree or in either overlay of
 * dtbo_a.img, so the pins are not muxed by a pinctrl-0 there: Android's own i2c driver does it from
 * scl-gpio-id/sda-gpio-id with the pad config at gpio_start, 0x10002600, and leaves the pull-ups on
 * for the bus. MT6768's pin table (mediatek,mt6768-pinfunc.h) gives pin 175 mode 0 as GPIO175 and
 * mode 1 as SCL7, pin 176 mode 0 as GPIO176 and mode 1 as SDA7 - so function 0 here is the GPIO the
 * bit-bang needs, and the pads' electrical setup is elsewhere (the iocfg block), left untouched.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/redwood_input.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

#define GPIO_PHYS               0x10005000ULL
#define GPIO_SIZE               0x1000ULL

/* The phone's device tree: i2c7's scl-gpio-id and sda-gpio-id */
#define PIN_SCL                 175u
#define PIN_SDA                 176u

/*
 * MT6768 pins, as the loader and the touch driver use them: a pin's function is four bits at
 * 0x300 + 0x10 per eight pins; direction, output and input are one bit a pin in 32-pin banks at
 * 0x004/0x104/0x200, four bytes of set and four of clear at the start of each.
 */
#define PIN_FUNC_GPIO           0u      /* pin 175/176: 0 is GPIO175/176, 1 is SCL7/SDA7 */
#define PIN_BANK(n)             ((n) / 32u)
#define PIN_BIT(n)              (1u << ((n) % 32u))
#define PIN_MODE(n)             (0x300u + 0x10u * ((n) / 8u))
#define PIN_MODE_SHIFT(n)       (4u * ((n) % 8u))
#define PIN_DIR(b)              (0x000u + 0x10u * (b))         /* a bank's own word, to read */
#define PIN_DIR_SET(b)          (0x004u + 0x10u * (b))
#define PIN_DIR_CLR(b)          (0x008u + 0x10u * (b))
#define PIN_DOUT(b)             (0x100u + 0x10u * (b))
#define PIN_DOUT_SET(b)         (0x104u + 0x10u * (b))
#define PIN_DOUT_CLR(b)         (0x108u + 0x10u * (b))
#define PIN_DIN(b)              (0x200u + 0x10u * (b))

/* The part's registers, from SLUSC86: read-only where this file reads them. */
#define BQ_ADDRESS              0x6a
#define BQ_REG_IINLIM           0x00
#define BQ_REG_ICHG             0x04
#define BQ_REG_VREG             0x06
#define BQ_REG_STATUS           0x0b
#define BQ_REG_FAULT            0x0c
#define BQ_REG_BATV             0x0e
#define BQ_REG_SYSV             0x0f
#define BQ_REG_TSPCT            0x10
#define BQ_REG_VBUSV            0x11
#define BQ_REG_ICHGR            0x12
#define BQ_REG_ID               0x14

static vm_offset_t gpio;

/* What the last reading said, for the sysctls and for the log's "nothing changed" line. */
static int bq_present = -1, bq_status = -1;
static int bq_vbus_mv = -1, bq_vbat_mv = -1, bq_ichg_ma = -1;
static int bq_iinlim_ma = -1, bq_ichg_limit_ma = -1, bq_vreg_mv = -1;

/* The last line the log printed, and the two ways a transaction can fail: the bus was not idle when
 * it was taken (a line low at rest, so nothing is driven), or a slave held a line low while the
 * clock was released (the clock stretch that never ended). Kept apart, because one says the board's
 * pull-ups are missing and the other says the part is wedged. */
static int bq_last_present = -2, bq_last_status = -2;
static bool bus_held, i2c_fault;

SYSCTL_NODE(_kern, OID_AUTO, bq2589x, CTLFLAG_RW | CTLFLAG_LOCKED, 0, "the Redmi 10's charger");
SYSCTL_INT(_kern_bq2589x, OID_AUTO, present, CTLFLAG_RD | CTLFLAG_LOCKED,
    &bq_present, 0, "1 while the part sees VBUS good, 0 without a cable, -1 before the first reading");
SYSCTL_INT(_kern_bq2589x, OID_AUTO, status, CTLFLAG_RD | CTLFLAG_LOCKED,
    &bq_status, 0, "Linux's charging status: 1 charging, 2 discharging, 3 not charging, 4 full");
SYSCTL_INT(_kern_bq2589x, OID_AUTO, vbus_mv, CTLFLAG_RD | CTLFLAG_LOCKED,
    &bq_vbus_mv, 0, "VBUS in millivolts (REG11, 2.6 V + 100 mV a count)");
SYSCTL_INT(_kern_bq2589x, OID_AUTO, vbat_mv, CTLFLAG_RD | CTLFLAG_LOCKED,
    &bq_vbat_mv, 0, "VBAT in millivolts (REG0E, 2.304 V + 20 mV a count)");
SYSCTL_INT(_kern_bq2589x, OID_AUTO, ichg_ma, CTLFLAG_RD | CTLFLAG_LOCKED,
    &bq_ichg_ma, 0, "charge current into the battery in milliamps (REG12, 50 mA a count)");
SYSCTL_INT(_kern_bq2589x, OID_AUTO, iinlim_ma, CTLFLAG_RD | CTLFLAG_LOCKED,
    &bq_iinlim_ma, 0, "the input current limit the part holds (REG00, 100 mA + 50 mA a count)");
SYSCTL_INT(_kern_bq2589x, OID_AUTO, ichg_limit_ma, CTLFLAG_RD | CTLFLAG_LOCKED,
    &bq_ichg_limit_ma, 0, "the fast-charge current limit the part holds (REG04, 64 mA a count)");
SYSCTL_INT(_kern_bq2589x, OID_AUTO, vreg_mv, CTLFLAG_RD | CTLFLAG_LOCKED,
    &bq_vreg_mv, 0, "the charge voltage limit the part holds (REG06, 3.84 V + 16 mV a count)");

static inline uint32_t
rd(uint32_t at)
{
	return *(volatile uint32_t *)(gpio + at);
}

static inline void
wr(uint32_t at, uint32_t value)
{
	*(volatile uint32_t *)(gpio + at) = value;
}

/* What a pin is muxed to: 0 is its GPIO, 1 I2C7's SCL/SDA on pins 175 and 176. */
static uint32_t
pin_func(unsigned pin)
{
	return (rd(PIN_MODE(pin)) >> PIN_MODE_SHIFT(pin)) & 0xfu;
}

/* ---- I2C by hand, open drain on two input/output pins ---------------------------------------------- */

static uint32_t saved_mode[2], saved_dir, saved_dout;
static unsigned pins[2] = { PIN_SCL, PIN_SDA };

/*
 * A line is driven low, or let go for the pull-ups to raise. Letting go clears the direction first
 * and only then sets the latch high: the other order drives the line high for a moment if it was
 * being held low, and that pulse is a false edge on the bus.
 */
static void
line(unsigned pin, bool high)
{
	uint32_t bank = PIN_BANK(pin), bit = PIN_BIT(pin);

	if (high) {
		wr(PIN_DIR_CLR(bank), bit);
		wr(PIN_DOUT_SET(bank), bit);
	} else {
		wr(PIN_DOUT_CLR(bank), bit);
		wr(PIN_DIR_SET(bank), bit);
	}
}

static bool
line_level(unsigned pin)
{
	return (rd(PIN_DIN(PIN_BANK(pin))) & PIN_BIT(pin)) != 0;
}

static void
half_bit(void)
{
	delay(5);                       /* 100 kHz at most, I2C's standard mode */
}

static void
bus_give_back(void)
{
	uint32_t bank = PIN_BANK(PIN_SCL), both = PIN_BIT(PIN_SCL) | PIN_BIT(PIN_SDA);

	/* as inputs first, so that the function going back cannot drive the bus for an instant */
	wr(PIN_DIR_CLR(bank), both);
	wr(PIN_MODE(PIN_SCL), saved_mode[0]);
	wr(PIN_MODE(PIN_SDA), saved_mode[1]);
	wr(PIN_DOUT_CLR(bank), both);
	wr(PIN_DOUT_SET(bank), saved_dout & both);
	wr(PIN_DIR_SET(bank), saved_dir & both);
}

/* Both pins taken as GPIOs, and false if either reads low at rest: then nothing is driven. */
static bool
bus_take(void)
{
	uint32_t bank = PIN_BANK(PIN_SCL), both = PIN_BIT(PIN_SCL) | PIN_BIT(PIN_SDA);

	saved_mode[0] = rd(PIN_MODE(PIN_SCL));
	saved_mode[1] = rd(PIN_MODE(PIN_SDA));
	saved_dir = rd(PIN_DIR(bank));
	saved_dout = rd(PIN_DOUT(bank));

	/* inputs first, then the latch high: nothing here can drive the bus, and if the function switch
	 * turns one of them into an output it drives high, never low */
	wr(PIN_DIR_CLR(bank), both);
	wr(PIN_DOUT_SET(bank), both);
	for (unsigned i = 0; i < 2; i++) {
		uint32_t mode = rd(PIN_MODE(pins[i]));

		mode &= ~(0xfu << PIN_MODE_SHIFT(pins[i]));
		mode |= PIN_FUNC_GPIO << PIN_MODE_SHIFT(pins[i]);
		wr(PIN_MODE(pins[i]), mode);
	}
	half_bit();
	if (!line_level(PIN_SCL) || !line_level(PIN_SDA)) {
		bus_give_back();
		bus_held = true;
		return false;
	}
	return true;
}

static void
i2c_start(void)
{
	line(PIN_SDA, true);
	line(PIN_SCL, true);
	half_bit();
	line(PIN_SDA, false);
	half_bit();
	line(PIN_SCL, false);
	half_bit();
}

static void
i2c_stop(void)
{
	line(PIN_SDA, false);
	half_bit();
	line(PIN_SCL, true);
	half_bit();
	line(PIN_SDA, true);
	half_bit();
}

static bool
i2c_bit(bool out)
{
	bool in;

	line(PIN_SDA, out);
	half_bit();
	line(PIN_SCL, true);
	for (unsigned stretched = 0; !line_level(PIN_SCL) && stretched < 100; stretched++) {
		half_bit();             /* a device may hold the clock low until it is ready */
	}
	if (!line_level(PIN_SCL)) {
		/*
		 * Half a millisecond and the clock is still low: a stuck bus, not a slow device. Give up
		 * instead of reading data off it - SDA low there would look exactly like an acknowledge,
		 * and every register would then come back as 0x00.
		 */
		i2c_fault = true;
		line(PIN_SCL, false);
		return false;
	}
	half_bit();
	in = line_level(PIN_SDA);
	line(PIN_SCL, false);
	return in;
}

static bool
i2c_write(uint8_t byte)
{
	for (int bit = 7; bit >= 0 && !i2c_fault; bit--) {
		i2c_bit((byte >> bit) & 1);
	}
	if (i2c_fault) {
		return false;
	}
	return !i2c_bit(true);
}

static uint8_t
i2c_read(void)
{
	uint8_t byte = 0;

	for (int bit = 7; bit >= 0 && !i2c_fault; bit--) {
		byte = (uint8_t)(byte << 1 | i2c_bit(true));
	}
	if (!i2c_fault) {
		/* The last byte of a read is not acknowledged, I2C's way of saying "that is all";
		 * the other way round tells the part it may keep sending. */
		(void)i2c_bit(true);
	}
	return byte;
}

/* One register of the charger, addressed the way the part's own register map is. */
static bool
reg_read(uint8_t reg, uint8_t *value)
{
	bool answered;

	bus_held = i2c_fault = false;
	if (!bus_take()) {
		return false;
	}
	i2c_start();
	answered = i2c_write(BQ_ADDRESS << 1) && i2c_write(reg);
	if (answered) {
		i2c_start();
		answered = i2c_write((BQ_ADDRESS << 1) | 1);
	}
	if (answered) {
		*value = i2c_read();
	}
	i2c_stop();
	bus_give_back();
	return answered && !i2c_fault;
}

/* ---- what the registers say ------------------------------------------------------------------------ */

static const char *
vbus_name(unsigned stat)
{
	static const char *names[] = {
		"no input", "SDP", "CDP", "DCP", "MaxCharge DCP", "unknown adapter", "non-standard adapter", "OTG"
	};

	return stat < 8 ? names[stat] : "?";
}

static const char *
chrg_name(unsigned stat)
{
	static const char *names[] = { "not charging", "pre-charge", "fast charge", "done" };

	return stat < 4 ? names[stat] : "?";
}

static const char *
part_name(unsigned pn)
{
	switch (pn) {
	case 3:
		return "BQ25890";
	case 7:
		return "BQ25895";
	case 0:
		return "BQ25896";
	default:
		return "not a BQ2589x part number";
	}
}

static const char *
bq_why(void)
{
	if (bus_held) {
		return "a line is low at rest, so nothing is driven onto it (no pull-up, or a slave holding the bus)";
	}
	if (i2c_fault) {
		return "the clock stayed low after it was released (a slave holding the bus)";
	}
	return "the address was not acknowledged";
}

/*
 * The part or the bus stopped answering: say why once, and put the sysctls back to "unknown"
 * instead of leaving the last reading there as if it were current.
 */
static void
bq_lost(const char *why)
{
	if (bq_last_present != -1) {
		printf("bq2589x: nothing to read at 0x%02x on I2C7: %s\n", BQ_ADDRESS, why);
		bq_last_present = -1;
	}
	bq_present = bq_status = -1;
	bq_vbus_mv = bq_vbat_mv = bq_ichg_ma = -1;
	bq_iinlim_ma = bq_ichg_limit_ma = bq_vreg_mv = -1;
}

static void
bq_read(uint32_t verbose)
{
	uint8_t iinlim, ichg, vreg, status, fault, batv, sysv, tspct, vbusv, ichgr, id;
	static int printed_part, printed_regs;
	int present, stat, vbus_mv, vbat_mv, ichg_ma;

	if (!reg_read(BQ_REG_ID, &id)) {
		bq_lost(bq_why());
		return;
	}
	if (id == 0x00 || id == 0xff) {
		/*
		 * A whole byte of one level: SDA held low reads as 0x00, a bus with nothing on it floats
		 * to 0xff - and both look like an acknowledge to a master that only counts ACKs.
		 */
		bq_lost(id == 0x00 ? "the part read back 0x00, SDA held low" :
		    "the part read back 0xff, a bus with nothing on it");
		return;
	}
	if (!printed_part) {
		printf("bq2589x: %s (%u) revision %u at 0x%02x on I2C7, read only\n",
		    part_name((id >> 3) & 7u), (id >> 3) & 7u, id & 3u, BQ_ADDRESS);
		printed_part = 1;
	}

	/* A register that does not answer alone is not worth the rest of the reading. */
	if (!reg_read(BQ_REG_STATUS, &status) || !reg_read(BQ_REG_FAULT, &fault) ||
	    !reg_read(BQ_REG_BATV, &batv) || !reg_read(BQ_REG_SYSV, &sysv) ||
	    !reg_read(BQ_REG_TSPCT, &tspct) || !reg_read(BQ_REG_VBUSV, &vbusv) ||
	    !reg_read(BQ_REG_ICHGR, &ichgr)) {
		bq_lost(bq_why());
		return;
	}
	if (!reg_read(BQ_REG_IINLIM, &iinlim) || !reg_read(BQ_REG_ICHG, &ichg) || !reg_read(BQ_REG_VREG, &vreg)) {
		bq_lost(bq_why());
		return;
	}

	/* The three the loader's charger probe reads by hand: same registers, same hex, for comparing
	 * what the two ways onto the bus see of the same part. */
	if (!printed_regs || verbose >= 2) {
		printf("bq2589x: REG0B %#x  REG0C %#x  REG14 %#x\n", status, fault, id);
		printed_regs = 1;
	}

	present = (vbusv >> 7) & 1;                     /* VBUS_GD */
	stat = (status >> 3) & 3;                       /* CHRG_STAT */
	vbus_mv = 2600 + 100 * (vbusv & 0x7f);
	vbat_mv = 2304 + 20 * (batv & 0x7f);
	ichg_ma = 50 * (ichgr & 0x7f);

	bq_present = present;
	bq_vbus_mv = vbus_mv;
	bq_vbat_mv = vbat_mv;
	bq_ichg_ma = ichg_ma;
	bq_iinlim_ma = 100 + 50 * (iinlim & 0x3f);
	bq_ichg_limit_ma = 64 * (ichg & 0x7f);
	bq_vreg_mv = 3840 + 16 * ((vreg >> 2) & 0x3f);
	/* Linux's numbers, as redwood_mtkbatt.c reports them: 1 charging, 2 discharging, 3 not charging, 4 full */
	if (!present) {
		bq_status = 2;
	} else if (stat == 3) {
		bq_status = 4;
	} else if (stat == 0) {
		bq_status = 3;
	} else {
		bq_status = 1;
	}

	if (verbose >= 2 || present != bq_last_present || stat != bq_last_status) {
		printf("bq2589x: %s, %s on %s (%u), vbus %d mV, vbat %d mV, ichg %d mA; "
		    "limits iin %d mA, ichg %d mA, vreg %d mV; sys %d mV, ts %u%%, fault %#x\n",
		    present ? "power good" : "no vbus", chrg_name(stat), vbus_name((status >> 5) & 7),
		    (status >> 5) & 7, vbus_mv, vbat_mv, ichg_ma, bq_iinlim_ma, bq_ichg_limit_ma,
		    bq_vreg_mv, 2304 + 20 * (sysv & 0x7f), tspct & 0x7f, fault);
		bq_last_present = present;
		bq_last_status = stat;
	}
}

/* ---- the driver's thread --------------------------------------------------------------------------- */

static void
bq_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint32_t level = 1;

	(void)PE_parse_boot_argn("bq2589x", &level, sizeof(level));
	gpio = ml_io_map(GPIO_PHYS, GPIO_SIZE);
	if (!gpio) {
		printf("bq2589x: cannot map the GPIO registers\n");
		return;
	}
	/* What the pins were muxed to before anything here runs: 0 is GPIO, 1 is SCL7/SDA7. */
	printf("bq2589x: the charger at 0x%02x on I2C7 (gpio%u clock in function %u, gpio%u data in "
	    "function %u), read every 5 s\n", BQ_ADDRESS, PIN_SCL, pin_func(PIN_SCL), PIN_SDA,
	    pin_func(PIN_SDA));

	for (;;) {
		bq_read(level);
		for (unsigned i = 0; i < 50; i++) {
			delay(100000);          /* 5 s, in steps delay() takes */
		}
	}
}

void
redwood_bq2589x_init(void)
{
	uint32_t enable = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("bq2589x", &enable, sizeof(enable)) || enable < 1) {
		return;
	}
	if (kernel_thread_start(bq_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
