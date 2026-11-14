/*
 * board.h: hardware facts for the Redmi 10 family (MT6769H / Helio G88).
 * Covers selene, selenes, and eos (Redmi 10, Redmi 10 Prime, Redmi 10 2022, Redmi Note 11 4G).
 *
 * Why #defines and not a table of structs: this code runs with the MMU off and no libc, and every
 * one of these numbers ends up in an instruction immediate.
 */

#ifndef MTKLOADER_BOARD_H
#define MTKLOADER_BOARD_H

#define BOARD_NAME             "selene"

/*
 * XNU's memory: 0x100000000-0x1bfff5fff is System RAM in the phone's own /proc/iomem with no
 * reserved region inside it -- 3 GiB.
 */
#define BOARD_XNU_PHYS_BASE    0x100000000ULL
#define BOARD_XNU_MEM_SIZE     0xbff00000ULL

/* 1080x2400, NT36672C, video mode. LK's framebuffer is at 0x7e605000, 24 MB (vramSize 0x17e8000). */
#define BOARD_PANEL_W          1080u
#define BOARD_PANEL_H          2400u
#define BOARD_LK_FB_BASE       0x7e605000ULL
#define BOARD_LK_FB_SIZE       0x17e8000ULL
#define BOARD_LK_FB_PITCH      4352u   /* 1088 pixels: width aligned to 32 */
#define BOARD_DISPLAY_SCALE    2

/*
 * Touch: Novatek NT36672C on SPI0 (0x1100a000, "mediatek,mt6765-spi", cs 0, 9.6 MHz), reset on
 * GPIO 92, interrupt on GPIO 1. The SPI pins are 25-28 at function 2.
 */
#define BOARD_TOUCH_TCM        0
#define BOARD_TOUCH_NAME       "novatek nt36672c"
#define BOARD_TOUCH_SPI_BASE   0x1100a000ULL
#define BOARD_TOUCH_RESET_GPIO 92u
#define BOARD_TOUCH_IRQ_GPIO   1u

/* Cortex-A75 at 2000 MHz (Helio G88) */
#define BOARD_BIG_MHZ          2000u
#define BOARD_BOOT_PARTITION_SIZE (64u << 20)

#endif /* MTKLOADER_BOARD_H */
