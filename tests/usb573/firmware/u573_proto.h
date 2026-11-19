/* SPDX-License-Identifier: GPL-3.0-or-later */
/* USB_2_573: the wire protocol between the board and its host (docs/PROTOCOL.md).
 *
 * Everything here is plain constants and byte offsets, so that the firmware and a host
 * driver can share the file. Multi-byte fields are little-endian.
 */
#ifndef U573_PROTO_H
#define U573_PROTO_H

#include <stdint.h>

/* pid.codes' vendor ID with its test product ID: for development only. A product ID of the
 * board's own must be asked for at https://pid.codes before boards are handed out. A host
 * should also check the product string and the raw interface's usage page. */
#define U573_USB_VID 0x1209
#define U573_USB_PID 0x0001
#define U573_USB_MANUFACTURER "USB_2_573"
#define U573_USB_PRODUCT "USB_2_573 System 573 cabinet interface"
#define U573_HID_USAGE_PAGE 0xff73 /* the raw interface's top-level usage page */

#define U573_PROTO_VERSION 1
#define U573_REPORT_SIZE 64 /* raw interface: input, output and feature reports */

/* ---- switches: one bit each, 1 = closed (the pin is pulled to ground) -------------------- */
/* Bits 0-15 are the System 573's register 0x1f400008 inverted (JAMMA pin in brackets) */
#define U573_SW_P2_LEFT (1u << 0)    /* X */
#define U573_SW_P2_RIGHT (1u << 1)   /* Y */
#define U573_SW_P2_UP (1u << 2)      /* V */
#define U573_SW_P2_DOWN (1u << 3)    /* W */
#define U573_SW_P2_B1 (1u << 4)      /* Z */
#define U573_SW_P2_B2 (1u << 5)      /* a */
#define U573_SW_P2_B3 (1u << 6)      /* b */
#define U573_SW_P2_START (1u << 7)   /* U */
#define U573_SW_P1_LEFT (1u << 8)    /* 20 */
#define U573_SW_P1_RIGHT (1u << 9)   /* 21 */
#define U573_SW_P1_UP (1u << 10)     /* 18 */
#define U573_SW_P1_DOWN (1u << 11)   /* 19 */
#define U573_SW_P1_B1 (1u << 12)     /* 22 */
#define U573_SW_P1_B2 (1u << 13)     /* 23 */
#define U573_SW_P1_B3 (1u << 14)     /* 24 */
#define U573_SW_P1_START (1u << 15)  /* 17 */
#define U573_SW_P1_B4 (1u << 16)     /* 25, EXT-IN 3 */
#define U573_SW_P1_B5 (1u << 17)     /* 26, EXT-IN 4 */
#define U573_SW_P1_B6 (1u << 18)     /* EXT-IN 5 only */
#define U573_SW_P2_B4 (1u << 19)     /* c, EXT-IN 7 */
#define U573_SW_P2_B5 (1u << 20)     /* d, EXT-IN 8 */
#define U573_SW_P2_B6 (1u << 21)     /* EXT-IN 9 only */
#define U573_SW_COIN1 (1u << 22)     /* 16 */
#define U573_SW_COIN2 (1u << 23)     /* T */
#define U573_SW_SERVICE (1u << 24)   /* R */
#define U573_SW_TEST (1u << 25)      /* 15, and the board's own TEST button */
#define U573_SW_OPT1 (1u << 26)      /* the board's option switches 1-4 (1 = on) */
#define U573_SW_OPT2 (1u << 27)
#define U573_SW_OPT3 (1u << 28)
#define U573_SW_OPT4 (1u << 29)
#define U573_SW_CABINET 0x03ffffffu  /* the 26 cabinet switches */
#define U573_SW_ALL 0x3fffffffu
#define U573_SW_COUNT 30

/* ---- outputs: a 48-bit state ------------------------------------------------------------- */
/* Bits 0-27: the I/O board's light outputs, 1 = the output sinks to its bank's common (lit) */
#define U573_OUT_A(n) (UINT64_C(1) << (n))        /* A0-A7 */
#define U573_OUT_B(n) (UINT64_C(1) << (8 + (n)))  /* B0-B7 */
#define U573_OUT_C(n) (UINT64_C(1) << (16 + (n))) /* C0-C7 */
#define U573_OUT_D(n) (UINT64_C(1) << (24 + (n))) /* D0-D3 */
#define U573_OUT_BANKS UINT64_C(0x0fffffff)
/* Bits 28-31: the security cartridge slot's D4-D7, the pin's logic level (1 = 5 V). D0-D3 are
 * left out on purpose: they are the cartridge's security chip, which a host must never write. */
#define U573_OUT_CART(n) (UINT64_C(1) << (28 + (n) - 4)) /* n = 4-7 */
#define U573_OUT_CART_ALL (UINT64_C(0xf) << 28)
/* Bits 32-39: EXT-OUT's OUT0-OUT7, the pin's logic level (1 = 5 V) */
#define U573_OUT_EXT(n) (UINT64_C(1) << (32 + (n)))
#define U573_OUT_EXT_ALL (UINT64_C(0xff) << 32)
/* Bits 40-41: the coin counters, 1 = the coil is energised */
#define U573_OUT_COUNTER1 (UINT64_C(1) << 40)
#define U573_OUT_COUNTER2 (UINT64_C(1) << 41)
#define U573_OUT_COUNTERS (U573_OUT_COUNTER1 | U573_OUT_COUNTER2)
#define U573_OUT_ALL (U573_OUT_BANKS | U573_OUT_CART_ALL | U573_OUT_EXT_ALL | U573_OUT_COUNTERS)
#define U573_OUT_BYTES 6

/* ---- raw interface: input report (board to host), every millisecond ---------------------- */
#define U573_IN_MAGIC 0x73
#define U573_IN_OFF_MAGIC 0
#define U573_IN_OFF_VERSION 1
#define U573_IN_OFF_STATUS 2
#define U573_IN_OFF_COUNTER 3   /* one more each report */
#define U573_IN_OFF_SWITCHES 4  /* u32 */
#define U573_IN_OFF_ANALOG 8    /* CH0-CH3, 0-255 for 0-5 V (the 573's ADC0834) */
#define U573_IN_OFF_APPLIED 12  /* u16: output entries applied since the last reset, wrapping */
#define U573_IN_OFF_QUEUED 14   /* entries waiting in the FIFO */
#define U573_IN_OFF_CAPACITY 15 /* the FIFO's size */
#define U573_IN_OFF_TIME 16     /* u32: the board's clock at the report, microseconds */
#define U573_IN_OFF_OUTPUTS 20  /* the outputs as they are driven now, 6 bytes */
#define U573_IN_USED 26

#define U573_STATUS_HOST_ACTIVE 0x01 /* a raw output report came within the failsafe time */
#define U573_STATUS_OVERFLOW 0x02    /* output entries were dropped: the FIFO was full */
#define U573_STATUS_COUNTER_CUT 0x04 /* a coin counter was held too long and was let go */

/* ---- raw interface: output report (host to board) ---------------------------------------- */
#define U573_OUT_MAGIC 0x53
#define U573_OUT_OFF_MAGIC 0
#define U573_OUT_OFF_CMD 1
#define U573_OUT_OFF_COUNT 2 /* CMD_OUTPUTS: entries that follow, 0-7 */
#define U573_OUT_OFF_FLAGS 3
#define U573_OUT_OFF_DATA 8

#define U573_CMD_KEEPALIVE 0x00 /* nothing but "the host is here" (every report says that) */
#define U573_CMD_OUTPUTS 0x01   /* output entries for the FIFO */
#define U573_CMD_CONFIG 0x02    /* u16 debounce_us, u16 failsafe_ms, u16 counter_max_ms */
#define U573_CMD_RESET 0x03     /* FIFO emptied, outputs off, counts and flags cleared */
#define U573_CMD_BOOTLOADER 0x7e /* data "BOOT": outputs off, then the board restarts as its USB
                                  * bootloader, for a new firmware (a drive that takes a .uf2) */

#define U573_OUTFLAG_FLUSH 0x01 /* CMD_OUTPUTS: what is still queued is dropped first */

/* An output entry: u16 delay, then the 48-bit state. The state is driven `delay` after the
 * entry before it was (10 microsecond units; 0 = at once), and never sooner than it arrives. */
#define U573_ENTRY_SIZE 8
#define U573_ENTRIES_MAX 7
#define U573_DELAY_UNIT_US 10

/* ---- raw interface: feature report (GET_REPORT): what the board is ------------------------ */
#define U573_INFO_OFF_MAGIC 0     /* "U573" */
#define U573_INFO_OFF_VERSION 4   /* protocol version */
#define U573_INFO_OFF_FW 5        /* firmware version: major, minor, patch */
#define U573_INFO_OFF_HW 8        /* hardware revision */
#define U573_INFO_OFF_CAPACITY 9  /* the FIFO's size */
#define U573_INFO_OFF_ANALOG 10   /* analog channels fitted (0-4) */
#define U573_INFO_OFF_DEBOUNCE 12 /* u16: microseconds */
#define U573_INFO_OFF_FAILSAFE 14 /* u16: milliseconds */
#define U573_INFO_OFF_COUNTER 16  /* u16: milliseconds */
#define U573_INFO_OFF_UID 20      /* 8 bytes: the board's own number */
#define U573_INFO_USED 28

/* ---- gamepad interface (for anything that takes a plain HID gamepad) ---------------------- */
/* Input, 8 bytes: buttons 1-26 are switch bits 0-25, then four 8-bit axes (X, Y, Z, Rx = CH0-3).
 * Output, 5 bytes: 38 one-bit lamps, in the order A0-A7, B0-B7, C0-C7, D0-D3, OUT0-OUT7, the two
 * coin counters (the cartridge slot's lines are the raw interface's alone). Ignored while a host
 * talks to the raw interface. */
#define U573_PAD_IN_SIZE 8
#define U573_PAD_OUT_SIZE 5
#define U573_PAD_LAMPS 38

#endif
