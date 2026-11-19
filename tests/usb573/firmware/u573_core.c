/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "u573_core.h"

#include <string.h>

static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)v), put16(p + 2, (uint16_t)(v >> 16)); }

static uint64_t get_state(const uint8_t *p) {
    uint64_t v = 0;
    for (int k = U573_OUT_BYTES - 1; k >= 0; k--) v = v << 8 | p[k];
    return v & U573_OUT_ALL;
}
static void put_state(uint8_t *p, uint64_t v) {
    for (int k = 0; k < U573_OUT_BYTES; k++) p[k] = (uint8_t)(v >> (8 * k));
}

void u573_init(u573_t *s, uint64_t now_us) {
    memset(s, 0, sizeof *s);
    s->debounce_us = U573_DEFAULT_DEBOUNCE_US;
    s->failsafe_ms = U573_DEFAULT_FAILSAFE_MS;
    s->counter_max_ms = U573_DEFAULT_COUNTER_MS;
    s->analog_channels = 4;
    s->applied_at = s->sampled_at = now_us;
}

/* ---- switches --------------------------------------------------------------------------- */

/* A change is taken at once, then that switch is left alone for the debounce time: a press
 * costs no time, and the contacts' bounce after it is not seen. */
void u573_sample(u573_t *s, uint64_t now_us, uint32_t raw) {
    raw &= U573_SW_ALL;
    uint32_t diff = raw ^ s->switches;
    for (int k = 0; diff; k++, diff >>= 1) {
        if (!(diff & 1)) continue;
        /* (changed_at 0: never changed, so a switch held at power-on is taken at the first sample) */
        if (s->changed_at[k] && now_us - s->changed_at[k] < s->debounce_us) continue;
        s->switches ^= 1u << k;
        s->changed_at[k] = now_us ? now_us : 1;
    }
    s->latch_raw |= s->switches;
    s->latch_pad |= s->switches;
    s->sampled_at = now_us;
}

void u573_set_analog(u573_t *s, const uint8_t ch[4]) { memcpy(s->analog, ch, 4); }

/* ---- outputs ---------------------------------------------------------------------------- */

static void flush(u573_t *s) { s->head = s->count = 0; }

static bool within(uint64_t now_us, uint64_t at, uint16_t ms) { return now_us - at < (uint64_t)ms * 1000; }

bool u573_host_active(const u573_t *s, uint64_t now_us) { return s->host_seen && within(now_us, s->host_at, s->failsafe_ms); }

void u573_bus_idle(u573_t *s) {
    s->host_seen = false;
    s->outputs = 0;
    flush(s);
}

static uint64_t due_at(const u573_t *s) { return s->applied_at + (uint64_t)s->fifo[s->head].delay * U573_DELAY_UNIT_US; }

/* What goes to the pins: the host's state, less the coin counters the board has let go */
static uint64_t driven(u573_t *s, uint64_t now_us) {
    static const uint64_t counter[2] = {U573_OUT_COUNTER1, U573_OUT_COUNTER2};
    for (int k = 0; k < 2; k++) {
        if (!(s->outputs & counter[k])) {
            s->cut &= ~counter[k], s->counter_since[k] = 0;   /* the host let go: it may pull again */
        } else if (!s->counter_since[k]) {
            s->counter_since[k] = now_us ? now_us : 1;
        } else if (!within(now_us, s->counter_since[k], s->counter_max_ms)) {
            s->cut |= counter[k];
            s->status |= U573_STATUS_COUNTER_CUT;
        }
    }
    return s->outputs & ~s->cut;
}

bool u573_task(u573_t *s, uint64_t now_us, uint64_t *drive) {
    const uint64_t was = *drive;
    if (s->host_seen && !within(now_us, s->host_at, s->failsafe_ms)) u573_bus_idle(s);   /* the host went away */
    if (s->count && now_us >= due_at(s)) {
        s->outputs = s->fifo[s->head].state;
        s->head = (uint16_t)((s->head + 1) % U573_FIFO_SIZE);
        s->count--;
        s->applied++;
        s->applied_at = now_us;
    }
    *drive = driven(s, now_us);
    return *drive != was;
}

uint64_t u573_next_due(const u573_t *s, uint64_t now_us) {
    uint64_t t = UINT64_MAX;
    if (s->count) t = due_at(s);
    if (s->host_seen) {
        const uint64_t f = s->host_at + (uint64_t)s->failsafe_ms * 1000;
        if (f < t) t = f;
    }
    for (int k = 0; k < 2; k++)
        if (s->counter_since[k] && !(s->cut & (k ? U573_OUT_COUNTER2 : U573_OUT_COUNTER1))) {
            const uint64_t c = s->counter_since[k] + (uint64_t)s->counter_max_ms * 1000;
            if (c < t) t = c;
        }
    return t < now_us ? now_us : t;
}

static void push(u573_t *s, uint64_t now_us, uint16_t delay, uint64_t state) {
    if (s->count >= U573_FIFO_SIZE) {
        s->status |= U573_STATUS_OVERFLOW;
        return;
    }
    if (!s->count) {
        /* the first of a run counts its delay from the entry before it, but an entry that
         * comes long after that one is not late: it goes out as it arrives */
        if (now_us - s->applied_at > (uint64_t)delay * U573_DELAY_UNIT_US) delay = 0;
    }
    u573_entry_t *e = &s->fifo[(s->head + s->count) % U573_FIFO_SIZE];
    e->delay = delay;
    e->state = state;
    s->count++;
}

/* ---- the raw interface ------------------------------------------------------------------ */

void u573_raw_out(u573_t *s, uint64_t now_us, const uint8_t *r, size_t len) {
    if (len < U573_OUT_OFF_DATA || r[U573_OUT_OFF_MAGIC] != U573_OUT_MAGIC) return;
    switch (r[U573_OUT_OFF_CMD]) {
    case U573_CMD_KEEPALIVE:
        break;
    case U573_CMD_OUTPUTS: {
        const unsigned n = r[U573_OUT_OFF_COUNT];
        if (n > U573_ENTRIES_MAX || len < U573_OUT_OFF_DATA + n * U573_ENTRY_SIZE) return;
        if (r[U573_OUT_OFF_FLAGS] & U573_OUTFLAG_FLUSH) flush(s);
        for (unsigned k = 0; k < n; k++) {
            const uint8_t *e = r + U573_OUT_OFF_DATA + k * U573_ENTRY_SIZE;
            push(s, now_us, get16(e), get_state(e + 2));
        }
        break;
    }
    case U573_CMD_CONFIG:
        if (len < U573_OUT_OFF_DATA + 6) return;
        s->debounce_us = get16(r + U573_OUT_OFF_DATA);
        s->failsafe_ms = get16(r + U573_OUT_OFF_DATA + 2);
        s->counter_max_ms = get16(r + U573_OUT_OFF_DATA + 4);
        if (s->failsafe_ms < 100) s->failsafe_ms = 100;   /* (never off: a host that stops must not leave a coil pulled) */
        if (s->counter_max_ms < 10) s->counter_max_ms = 10;
        break;
    case U573_CMD_RESET:
        flush(s);
        s->outputs = 0;
        s->applied = 0;
        s->applied_at = now_us;
        s->status = 0;
        break;
    case U573_CMD_BOOTLOADER:
        if (len < U573_OUT_OFF_DATA + 4 || memcmp(r + U573_OUT_OFF_DATA, "BOOT", 4)) return;
        u573_bus_idle(s);
        s->bootloader = true;
        return;
    default:
        return;   /* not a command of this protocol: not a sign of a host that speaks it */
    }
    s->host_seen = true;
    s->host_at = now_us;
}

void u573_raw_in(u573_t *s, uint64_t now_us, uint8_t r[U573_REPORT_SIZE]) {
    memset(r, 0, U573_REPORT_SIZE);
    r[U573_IN_OFF_MAGIC] = U573_IN_MAGIC;
    r[U573_IN_OFF_VERSION] = U573_PROTO_VERSION;
    r[U573_IN_OFF_STATUS] = (uint8_t)(s->status | (u573_host_active(s, now_us) ? U573_STATUS_HOST_ACTIVE : 0));
    r[U573_IN_OFF_COUNTER] = s->in_counter++;
    /* a switch that closed and opened again between two reports is still told, once */
    put32(r + U573_IN_OFF_SWITCHES, s->switches | s->latch_raw);
    s->latch_raw = 0;
    memcpy(r + U573_IN_OFF_ANALOG, s->analog, 4);
    put16(r + U573_IN_OFF_APPLIED, s->applied);
    r[U573_IN_OFF_QUEUED] = (uint8_t)(s->count > 255 ? 255 : s->count);
    r[U573_IN_OFF_CAPACITY] = U573_FIFO_SIZE;
    put32(r + U573_IN_OFF_TIME, (uint32_t)now_us);
    put_state(r + U573_IN_OFF_OUTPUTS, s->outputs & ~s->cut);
}

void u573_info(const u573_t *s, uint8_t r[U573_REPORT_SIZE]) {
    memset(r, 0, U573_REPORT_SIZE);
    memcpy(r + U573_INFO_OFF_MAGIC, "U573", 4);
    r[U573_INFO_OFF_VERSION] = U573_PROTO_VERSION;
    r[U573_INFO_OFF_FW] = U573_FW_MAJOR, r[U573_INFO_OFF_FW + 1] = U573_FW_MINOR, r[U573_INFO_OFF_FW + 2] = U573_FW_PATCH;
    r[U573_INFO_OFF_HW] = s->hw_rev;
    r[U573_INFO_OFF_CAPACITY] = U573_FIFO_SIZE;
    r[U573_INFO_OFF_ANALOG] = s->analog_channels;
    put16(r + U573_INFO_OFF_DEBOUNCE, s->debounce_us);
    put16(r + U573_INFO_OFF_FAILSAFE, s->failsafe_ms);
    put16(r + U573_INFO_OFF_COUNTER, s->counter_max_ms);
    memcpy(r + U573_INFO_OFF_UID, s->uid, 8);
}

/* ---- the gamepad interface -------------------------------------------------------------- */

void u573_pad_in(u573_t *s, uint8_t r[U573_PAD_IN_SIZE]) {
    put32(r, (s->switches | s->latch_pad) & U573_SW_CABINET);
    s->latch_pad = 0;
    memcpy(r + 4, s->analog, 4);
}

void u573_pad_out(u573_t *s, uint64_t now_us, const uint8_t *r, size_t len) {
    if (len < U573_PAD_OUT_SIZE) return;
    if (u573_host_active(s, now_us)) return;   /* a host that speaks the board's own protocol has the outputs */
    uint64_t lamps = 0;
    for (int k = U573_PAD_OUT_SIZE - 1; k >= 0; k--) lamps = lamps << 8 | r[k];
    lamps &= (UINT64_C(1) << U573_PAD_LAMPS) - 1;
    /* lamps 0-27: the banks; 28-35: EXT-OUT; 36-37: the coin counters */
    flush(s);
    s->outputs = (lamps & U573_OUT_BANKS) | (lamps >> 28 & 0xff) << 32 | (lamps >> 36 & 3) << 40;
}
