# The charger on the Redmi 10 (selene): BQ2589x on I2C7

What the phone itself says about its charger, what has been checked since, and the order the driver
is written in. The facts come first; where the work stands is a section of its own near the end,
because it moves while the facts do not.

Everything below is read from the phone's own files, not from a datasheet's example circuit:

* `hakios-selene/fw-selene/fdt.dtb` — the device tree LK hands the kernel (walked with a small FDT
  parser, node paths and property bytes quoted as they were read).
* `hakios-selene/fw-selene/dmesg.txt` — the last Android boot, where the charger driver logged what
  it programmed.

## The part, and where it sits

| | |
| --- | --- |
| Node | `/i2c7@11004000/bq2589x-charger@6A` |
| `compatible` | `ti,bq25890` |
| `reg` | `0x6a` — I2C address 6AH, the BQ25890's own address in TI's register map |
| `status` | `okay` |
| `charger_name` | `primary_chg` |
| `interrupt-names` | `chr_stat` |
| `ti,irq-gpio` | GPIO 20, flags 0 — active high as written (`<&pio 20 0>`) |
| `ti,chg-en-gpio` | GPIO 152 (0x98), flags 0 |
| `interrupts` | `<20 2 20 0>` — SPI-encoded, the same GPIO 20, falling edge |

Three more chargers sit on the same bus and are also `okay` — `sc8551-charger@66`,
`smb1351-charger@55`, `ln8000_charger@51` — plus a second `bq2589x`-shaped node is not present. Only
the BQ2589x logs as the working one (below), so it is the one to port first: `primary_chg`.

**There is no RT9471 on this phone.** The README and the battery driver said there was, on I2C5.
`rt9471` does not occur once in `fdt.dtb` or `dtbo_a.img` (`grep -ac rt9471` is 0 for both), and no
node on I2C5 looks like a charger: that bus holds the FUSB302/303 Type-C parts, the NXP NFC chip and
the `subpmic_pmu`. The claim was carried over from another port; it is corrected in `README.md`,
`README.ru.md` and `redwood_mtkbatt.c`.

## The bus

`/i2c7@11004000`, `compatible = "mediatek,i2c"`, register block `0x11004000` (length `0x1000`) and a
second range at `0x11000680` (length `0x180`), both in the node's `reg`.

| Property | Value | Means |
| --- | --- | --- |
| `id` | 7 | I2C7 |
| `clock-frequency` | `0x61a80` = 400000 | 400 kHz |
| `clock-div` | 5 | the divider the vendor driver starts from |
| `scl-gpio-id` | 175 | SCL on GPIO 175 |
| `sda-gpio-id` | 176 | SDA on GPIO 176 |
| `mediatek,use-open-drain` | present | open-drain, pulled up by the host or the board |
| `eh_cfg` / `pu_cfg` / `rsel_cfg` | `0x30` / `0x60` / `0x90` | the vendor driver's pin config values for those two pins |
| `gpio_start` | `0x10002600` | IO config block base the vendor driver writes through |

MT6769 pin registers, as the loader already uses them (`mtkloader/mtkspi.h`): `GPIO_MODE<n>` is four
bits a pin at `0x10005300 + 0x10` per eight pins, so GPIO 175 is in the group word at `0x10005450`
and GPIO 176 in the one at `0x10005460`. Direction is `DIR5_SET` (`0x004 + 0x10 * 5` = `0x054`),
output `DOUT5_SET` = `0x154`, input `DIN5` = `0x200 + 0x10 * 5` = `0x250`.

No pinctrl group covers those pins: neither the base tree nor either overlay in `dtbo_a.img` has a
`pinmux` entry for pin 175 or 176 (the base tree has 110 groups, none of them I2C7; the only
`pinmux` values in the overlays are for GPIO 152, the charger's enable). The two pins are therefore
muxed by Android's own i2c driver from `scl-gpio-id`/`sda-gpio-id`, not by a `pinctrl-0`. MT6768's
pin table
(`mediatek,mt6768-pinfunc.h`) gives the functions that matter:

| Pin | Mode 0 | Mode 1 |
| --- | --- | --- |
| 175 | GPIO175 | SCL7 |
| 176 | GPIO176 | SDA7 |

So mode 0 is the plain GPIO a bit-banged bus needs, and mode 1 is the I2C7 function Android leaves
the pins in. (Pins 198/199 are a second SCL7/SDA7 pair; this phone's tree names 175/176.) The pad's
electrical setup lives in the IO config block at `0x10002600`, which the vendor driver writes and a
bit-bang driver should not: `pu_cfg`/`rsel_cfg` say the vendor driver turns the internal pull-ups on
when it takes the pins.
Whether the board also has external resistors is **not known from these files** — it decides whether
a bit-banged bus can be read at all, so it is the first thing the probe below has to answer.

## What Android programmed through it

From `dmesg.txt`, the working charger's own log lines:

```
[bq2589x]:bq2589x_set_ichg: charge curr = 2944000
[bq2589x]:bq2589x_set_vchg: charge volt = 4450000
[bq2589x]:bq2589x_charging: enable charger successfully
swchg_select_cv, constant_voltage  = 4450000
chg1:1 chg2:0 chg2_chip_en:0
```

So Android charges at 2.944 A with a 4.45 V limit, and the second charge path ("chg2") is off.
`healthd` alongside it reports `v=4331 t=40.7 c=-1409300` (µV, °C, µA), which is what
the percentages in `redwood_mtkbatt.c` were matched against. The charger framework also logs
`power_supply charger: driver failed to report 'constant_charge_current_max': -22` repeatedly; that
is Android's own mtk_charger complaining about a property the BQ2589x driver does not implement, and
is not a fault of the part.

## Register map to read first

Straight from the BQ25890 datasheet (SLUSC86), all read-only except where noted. Address 6AH.

| Register | Bits | Field | Scale |
| --- | --- | --- | --- |
| `0x00` | 5:0 | `IINLIM` | 100 mA + 50 mA × N (100 mA .. 3.25 A) |
| `0x00` | 7 / 6 | `EN_HIZ` / `EN_ILIM` | |
| `0x04` | 6:0 | `ICHG` | 64 mA × N (0 .. 5056 mA) |
| `0x06` | 7:2 | `VREG` | 3840 mV + 16 mV × N (3840 .. 4608 mV) |
| `0x0B` | 7:5 | `VBUS_STAT` | 000 no input, 001 SDP, 010 CDP, 011 DCP, 100 MaxCharge, 101 unknown, 110 non-standard, 111 OTG |
| `0x0B` | 4:3 | `CHRG_STAT` | 00 not charging, 01 pre-charge, 10 fast, 11 done |
| `0x0B` | 2 | `PG_STAT` | power good |
| `0x0B` | 0 | `VSYS_STAT` | in VSYSMIN regulation |
| `0x0C` | 7 / 6 | `WD_FAULT` / `BOOST_FAULT` | |
| `0x0C` | 5:4 | `CHG_FAULT` | four states; the datasheet's names are in SLUSC86 table 9-21 |
| `0x0C` | 3 / 2:0 | `BAT_FAULT` / `NTC_FAULT` | |
| `0x0E` | 7 / 6:0 | `THERM_STAT` / `BATV` | 2304 mV + 20 mV × N |
| `0x0F` | 6:0 | `SYSV` | 2304 mV + 20 mV × N |
| `0x10` | 6:0 | `TSPCT` | TS pin, percent of REGN |
| `0x11` | 7 / 6:0 | `VBUS_GD` / `VBUSV` | 2600 mV + 100 mV × N |
| `0x12` | 6:0 | `ICHGR` | 50 mA × N (0 .. 6350 mA) |
| `0x13` | 7 / 6 / 5:0 | `VDPM_STAT` / `IDPM_STAT` / `IDPM_LIM` | limit as `IINLIM`: 100 mA + 50 mA × N |
| `0x14` | 7 / 6 / 5:3 / 2 / 1:0 | `REG_RST` / `ICO_OPTIMIZED` / `PN` / `TS_PROFILE` / `DEV_REV` | `PN`: 011 = BQ25890, 111 = BQ25895, 000 = BQ25896 |

Reads return the last ADC conversion, which the chip refreshes on its own rate — one second unless
`CONV_RATE` (REG02 bit 6) asks for continuous — so nothing has to be started and no register has to
be written to get a reading.

## The order of work, with the check for each step

1. **Confirm the pins without touching the part.** Read the `GPIO_MODE` group for GPIO 175/176
   (`0x10005450`/`0x10005460`), `DIR5`, `DOUT5`, `DIN5` and print them, before anything drives a
   line. *Check:* both pins report function 1 (SCL7/SDA7, the I2C function Android leaves them in)
   and both read high in `DIN5` — high means pull-ups that work, which is what makes a bit-banged
   bus possible at all. The skeleton logs the two function numbers it finds for exactly this; a
   pin already in function 0 means something else has taken the bus.
2. **Read, do not write.** Bit-bang a read of `0x0B`, `0x0C`, `0x14` and print them. *Check:*
   something acknowledges 0x6A and `PN` says BQ25890 (011). If the bus is busy (a line low at rest),
   leave it alone and say so, as `redwood_speaker.c` does.
3. **The numbers.** Read `0x00`, `0x04`, `0x06`, `0x0E`–`0x13` and print them decoded with the scales
   above. *Check:* `VREG` is 4.45 V and `ICHG` 2.944 A — the values Android programmed — and `BATV`
   is within a few tens of mV of the MT6358's reading in `redwood_mtkbatt.c`. Two independent
   readings that agree is what makes the register map trustworthy on this board.
4. **Feed the battery driver.** Only after step 3 agrees: `VBUS_GD`/`PG_STAT` become the "charger
   present" answer, `CHRG_STAT` the "charging" answer, and `ICHGR` the current, replacing the
   `RGS_CHRDET` bit that `redwood_mtkbatt.c` reads today. *Check:* the status bar's charging bolt
   appears and disappears with the cable on a screen the battery driver already updates.
5. **Then, if ever, write.** Current and voltage limits are Android's policy (`mtk_charger` + JEITA),
   not something this kernel needs to invent; a write path is a separate decision, and until then
   the chip keeps the limits it already holds.

## Where this stands

Done, and in the tree:

* The part, its node, its address and the other chargers on the bus, from the device tree and
  Android's log. No RT9471 on this phone, in either file.
* I2C7's pins: 175 and 176, no pinctrl group for them in the base tree or in either overlay of
  `dtbo_a.img`, and MT6768's pin table putting them in function 0 as GPIO and function 1 as
  SCL7/SDA7.
* `mtkloader`'s `CHG_PROBE`, off by default so the normal loader is unchanged: build it with
  `mtkloader/build.sh -DCHG_PROBE=1`, flash slot a, and it prints the pins as Android left them,
  reads `REG0B`, `REG0C` and `REG14` by hand, puts the pins back and starts Android, so the log comes
  back through pstore.
* `xnu/files/bsd/dev/redwood_bq2589x.c`: a read-only driver behind `bq2589x=1`, bit-banging the same
  two pins, logging the same three registers in the same hex (once, or every reading with
  `bq2589x=2`) and then the decoded readings; wired into `redwood_input.c` and `redwood.patch`.
  The same numbers are on `kern.bq2589x` (`present`, `status`, `vbus_mv`, `vbat_mv`, `ichg_ma`,
  `iinlim_ma`, `ichg_limit_ma`, `vreg_mv`), so a boot with it on can be read without the log.

Not done yet:

* Neither has run on the phone. The probe has not been flashed, and the driver has never been
  compiled into a kernel — there is no XNU source tree or SDK on this workstation.
* Nothing writes to the part, so charging is still Android's and the status bar's answer comes from
  `redwood_mtkbatt.c` as before.

The next run with the phone connected: the probe first, then the driver, then compare their `REG0B`,
`REG0C` and `REG14` lines — the same registers read over the same two pins by two different pieces of
code is the check that the bit-bang is right. After that, step 4 of the order of work.

## Why the driver code is a read-only skeleton

`xnu/files/bsd/dev/redwood_bq2589x.c` is that first step as kernel code: boot-arg `bq2589x=1`
(default off), bit-bang on GPIO 175/176, reads only, refuses to drive if either line is low at rest.
What is still open, and why it stops at reading:

* The pins are now settled from two files - no `pinctrl-0` for I2C7 anywhere in the base tree or in
  `dtbo_a.img`, and MT6768's pin table says 175/176 are GPIO175/GPIO176 in mode 0 and SCL7/SDA7 in
  mode 1. What the skeleton cannot know from here is which function the pins are actually in when
  XNU starts, and whether the board pulls them up; it prints the former and tests the latter by not
  driving.
* Nothing in the tree bit-bangs I2C on an MT6769: `redwood_speaker.c` and `redwood_haptic.c` are the
  Qualcomm phone (`TLMM` at `0x0f100000`, GPIO 12/13), not this one, so there was no helper to copy.
* There is no XNU source tree or SDK on this workstation (`xnu-src/`, `xnu-deps/sdk/` are absent), so
  the driver has only been syntax-checked against stub headers, never compiled into a kernel, and
  the Redmi 10 is not connected to run a loader probe or a boot.

Steps 1 and 2 are the probe and the driver above; see "Where this stands" for what has run and what
has not.
