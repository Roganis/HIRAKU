/* SPDX-License-Identifier: GPL-3.0-or-later */
/* USB_2_573's firmware without its hardware: the switches' debounce, the timed output FIFO,
 * the failsafe, and the USB reports' contents. The platform (firmware/rp2040) samples the
 * switches, moves the reports and drives whatever state u573_task hands it. Built for the
 * PC too, where firmware/tests runs it.
 *
 * Time is the platform's clock in microseconds, never going back.
 */
#ifndef U573_CORE_H
#define U573_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "u573_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

#define U573_FW_MAJOR 0
#define U573_FW_MINOR 1
#define U573_FW_PATCH 0

#define U573_FIFO_SIZE 128 /* output entries; a power of two, at most 255 */

/* The defaults a host may change (U573_CMD_CONFIG); they come back at power-on */
#define U573_DEFAULT_DEBOUNCE_US 2000 /* a switch that changed is left alone that long */
#define U573_DEFAULT_FAILSAFE_MS 1000 /* without a host report: every output off */
#define U573_DEFAULT_COUNTER_MS 1000  /* a coin counter's coil is let go after that long */

typedef struct {
    uint16_t delay; /* U573_DELAY_UNIT_US units after the entry before */
    uint64_t state;
} u573_entry_t;

typedef struct {
    /* settings */
    uint16_t debounce_us, failsafe_ms, counter_max_ms;
    uint8_t hw_rev, analog_channels;
    uint8_t uid[8];

    /* switches */
    uint32_t switches;                   /* debounced, 1 = closed */
    uint32_t latch_raw, latch_pad;       /* closed at some time since that interface's last report */
    uint64_t changed_at[U573_SW_COUNT];  /* when each switch last changed */
    uint64_t sampled_at;
    uint8_t analog[4];

    /* outputs */
    uint64_t outputs;                    /* what the host asked for last (the FIFO's last applied entry) */
    uint64_t cut;                        /* coin counters let go by the board: off until the host lets go too */
    uint64_t counter_since[2];
    u573_entry_t fifo[U573_FIFO_SIZE];
    uint16_t head, count;                /* the entry due next, entries waiting */
    uint64_t applied_at;                 /* when the last entry was driven */
    uint16_t applied;                    /* entries driven since the last reset */

    /* the host: one that speaks the raw interface's protocol */
    bool host_seen;                      /* a report of its came, and the failsafe hasn't run since */
    uint64_t host_at;                    /* when its last report came */
    uint8_t status;                      /* the sticky U573_STATUS_* bits */
    bool bootloader;                     /* U573_CMD_BOOTLOADER came: the platform restarts into its bootloader */
    uint8_t in_counter;
} u573_t;

void u573_init(u573_t *s, uint64_t now_us);

/* The switches as read now (1 = closed, U573_SW_* bits) and the analog channels (0-255) */
void u573_sample(u573_t *s, uint64_t now_us, uint32_t raw_switches);
void u573_set_analog(u573_t *s, const uint8_t channels[4]);

/* The outputs to drive now (U573_OUT_* bits). Call it again and again: it hands the FIFO's
 * entries on one at a time, each when it is due, so that every state reaches the pins in
 * order. true: `drive` changed since the last call. */
bool u573_task(u573_t *s, uint64_t now_us, uint64_t *drive);
/* When u573_task next has something to do without a report coming (UINT64_MAX: never) */
uint64_t u573_next_due(const u573_t *s, uint64_t now_us);

/* The raw interface's reports. An output report that isn't one of the protocol's is ignored. */
void u573_raw_out(u573_t *s, uint64_t now_us, const uint8_t *report, size_t len);
void u573_raw_in(u573_t *s, uint64_t now_us, uint8_t report[U573_REPORT_SIZE]);
void u573_info(const u573_t *s, uint8_t report[U573_REPORT_SIZE]);

/* The gamepad interface's reports */
void u573_pad_in(u573_t *s, uint8_t report[U573_PAD_IN_SIZE]);
void u573_pad_out(u573_t *s, uint64_t now_us, const uint8_t *report, size_t len);

/* Whether a host has talked to the raw interface within the failsafe time (the status LED).
 * While one does, the outputs are its own: when it stops, they all go off (the failsafe).
 * What the gamepad interface lit stays lit: programs that drive plain HID lamps write only
 * when a lamp changes. */
bool u573_host_active(const u573_t *s, uint64_t now_us);
/* The USB bus went away (unplugged, suspended, the PC off): every output off */
void u573_bus_idle(u573_t *s);

#ifdef __cplusplus
}
#endif

#endif
