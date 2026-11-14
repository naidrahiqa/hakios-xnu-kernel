/*
 * board.h: hardware facts for the Redmi 10 family (MT6769H: selene, selenes, eos).
 *
 * Defaults to BOARD_SELENE (Redmi 10, Redmi 10 Prime, Redmi 10 2022, Redmi Note 11 4G).
 * Build with -DBOARD_EOS for the China variant (with NFC).
 *
 * Why #defines and not a table of structs: this code runs with the MMU off and no libc, and every
 * one of these numbers ends up in an instruction immediate. A struct would put a load in front of
 * each use and a relocation in the linker script's path for nothing.
 */

#ifndef MTKLOADER_BOARD_H
#define MTKLOADER_BOARD_H

#if defined(BOARD_EOS)

/* ---- Redmi Note 11 4G China (eos, MT6769H, 6 GiB) ----
 *
 * selene's China variant. Same SoC, same panel, same memory layout as selene, plus NFC
 * (mediatek,nfc on i2c3 at 0x1100f000, address 0x08). See docs/porting-mt6769-redmi.md.
 */
#define BOARD_NAME          "eos"

#define BOARD_XNU_PHYS_BASE 0x100000000ULL
#define BOARD_XNU_MEM_SIZE  0xbff00000ULL

#define BOARD_PANEL_W       1080u
#define BOARD_PANEL_H       2400u
#define BOARD_LK_FB_BASE    0x7e605000ULL
#define BOARD_LK_FB_SIZE    0x17e8000ULL
#define BOARD_LK_FB_PITCH   4352u   /* 1088 pixels: the width aligned to 32, as LK leaves it */
#define BOARD_DISPLAY_SCALE 2

#define BOARD_TOUCH_TCM     0
#define BOARD_TOUCH_NAME    "novatek nt36672c"
#define BOARD_TOUCH_SPI_BASE 0x1100a000ULL
#define BOARD_TOUCH_RESET_GPIO 92u
#define BOARD_TOUCH_IRQ_GPIO   1u

#define BOARD_NFC_PRESENT   1
#define BOARD_NFC_I2C_BASE  0x1100f000ULL
#define BOARD_NFC_I2C_ADDR  0x08u

#define BOARD_BIG_MHZ       2000u
#define BOARD_BOOT_PARTITION_SIZE (64u << 20)

#elif defined(BOARD_X6826B)

/* ---- Legacy Infinix Hot 20 (X6826B, MT6768) ---- */
#define BOARD_NAME          "x6826b"

#define BOARD_XNU_PHYS_BASE 0xc0000000ULL
#define BOARD_XNU_MEM_SIZE  0xfe000000ULL

#define BOARD_PANEL_W       720u
#define BOARD_PANEL_H       1640u
#define BOARD_LK_FB_BASE    0x787b0000ULL
#define BOARD_LK_FB_SIZE    0x1050000ULL
#define BOARD_LK_FB_PITCH   2944u
#define BOARD_DISPLAY_SCALE 2

#define BOARD_TOUCH_TCM     1
#define BOARD_TOUCH_NAME    "td4160"
#define BOARD_TOUCH_SPI_BASE 0x11012000ULL
#define BOARD_TOUCH_RESET_GPIO 0u
#define BOARD_TOUCH_IRQ_GPIO   0u

#define BOARD_NFC_PRESENT   0

#define BOARD_BIG_MHZ       1176u
#define BOARD_BOOT_PARTITION_SIZE (32u << 20)

#else /* BOARD_SELENE is the default */

/* ---- Redmi 10, Redmi 10 Prime, Redmi 10 2022, Redmi Note 11 4G (selene, selenes) ----
 *
 * Measured on a Redmi 10 (21061119AG, MT6769H, 6 GiB), 14 November 2026: its merged device tree,
 * /proc/iomem, /proc/partitions and boot_a. See docs/porting-mt6769-redmi.md.
 */
#define BOARD_NAME          "selene"

/*
 * XNU's memory: 0x100000000-0x1bfff5fff is System RAM in the phone's own /proc/iomem with no
 * reserved region inside it -- 3 GiB.
 */
#define BOARD_XNU_PHYS_BASE 0x100000000ULL
#define BOARD_XNU_MEM_SIZE  0xbff00000ULL

/* 1080x2400, NT36672C, video mode. LK's framebuffer is at 0x7e605000, 24 MB (vramSize 0x17e8000). */
#define BOARD_PANEL_W       1080u
#define BOARD_PANEL_H       2400u
#define BOARD_LK_FB_BASE    0x7e605000ULL
#define BOARD_LK_FB_SIZE    0x17e8000ULL
#define BOARD_LK_FB_PITCH   4352u   /* 1088 pixels: the width aligned to 32, as LK leaves it */
#define BOARD_DISPLAY_SCALE 2

/*
 * Touch: a Novatek NT36672C on SPI0 (0x1100a000, "mediatek,mt6765-spi", cs 0, 9.6 MHz), reset on
 * GPIO 92, interrupt on GPIO 1. The SPI pins are 25-28 at function 2.
 */
#define BOARD_TOUCH_TCM     0
#define BOARD_TOUCH_NAME    "novatek nt36672c"
#define BOARD_TOUCH_SPI_BASE 0x1100a000ULL
#define BOARD_TOUCH_RESET_GPIO 92u
#define BOARD_TOUCH_IRQ_GPIO   1u

#define BOARD_NFC_PRESENT   0

/* Cortex-A75 at 2000 MHz (Helio G88) */
#define BOARD_BIG_MHZ       2000u
#define BOARD_BOOT_PARTITION_SIZE (64u << 20)

#endif

#endif /* MTKLOADER_BOARD_H */
