/* knocturn_core.c - see knocturn_core.h */
#include "knocturn_core.h"
#include <string.h>

void nc_reset_cache(nc_state *s) {
    for (int i = 0; i < 128; i++) s->out_cache[i] = -1;
    s->out_len = 0;
}

void nc_init(nc_state *s, const nc_config *cfg, nc_usb_cb usb, nc_midi_cb midi, void *ctx) {
    memset(s, 0, sizeof *s);
    s->cfg = *cfg;
    s->usb = usb; s->midi = midi; s->ctx = ctx;
    for (int i = 0; i < 9; i++) s->enc_value[i] = 64;
    for (int p = 0; p < NC_NUM_PORTS; p++) for (int i = 0; i < 128; i++) s->shadow[p][i] = -1;
    nc_reset_cache(s);
}

void nc_flush(nc_state *s) {
    if (s->out_len > 0) {
        if (s->usb) s->usb(s->ctx, s->out_buf, s->out_len);
        s->out_len = 0;
    }
}

/* original driver output (Write)(const MIDI::Event&): dedupe per control, pairs (cc,val), max 8 bytes */
void nc_write(nc_state *s, uint8_t cc, uint8_t value) {
    if (cc > 0x7F) return;
    if (s->out_cache[cc] == value) return;
    s->out_cache[cc] = value;
    if (s->out_len > NC_MAX_PACKET - 2) nc_flush(s);
    s->out_buf[s->out_len++] = cc;
    s->out_buf[s->out_len++] = value & 0x7F;
}

static void raw_packet(nc_state *s, const uint8_t *p, size_t n) {
    nc_flush(s);
    if (s->usb) s->usb(s->ctx, p, n);
}

/* original driver output (Initialise) */
#ifndef NC_SLEEP_MS
#include <unistd.h>
#define NC_SLEEP_MS(ms) usleep((ms) * 1000)
#endif
void nc_send_init(nc_state *s) {
    static const uint8_t p1[] = { 0xB0, 0x00, 0x00 };
    static const uint8_t p2[] = { 0x28, 0x00, 0x2B, 0x4A, 0x2C, 0x00, 0x2E, 0x35 };
    static const uint8_t p3[] = { 0x2A, 0x02, 0x2C, 0x72, 0x2E, 0x30 };
    raw_packet(s, p1, sizeof p1);
    raw_packet(s, p2, sizeof p2);
    NC_SLEEP_MS(50);                      /* usleep(50000) in the original */
    raw_packet(s, p3, sizeof p3);
    nc_reset_cache(s);
}

void nc_clear_leds(nc_state *s) {
    for (int i = 0; i < 8; i++) nc_write(s, NC_LED_RING_FIRST + i, 0);
    for (int i = 0; i < 16; i++) nc_write(s, NC_LED_BUTTON_FIRST + i, 0);
    nc_flush(s);
}

static void local_led(nc_state *s, uint8_t cc, uint8_t v) {
    s->shadow[s->mode][cc] = v;
    nc_write(s, cc, v);
    nc_flush(s);
}

static int rel_to_delta(int v) { return v <= 0x40 ? v : v - 0x80; } /* as in Translate() */

/* original control table (HandleEncoder): speed-sensitive scaling */
static int accelerate(nc_state *s, int idx, int delta, uint64_t now_ns) {
    uint64_t dt_ms = s->last_time[idx] ? (now_ns - s->last_time[idx]) / 1000000ULL : 1000;
    s->last_time[idx] = now_ns;
    int dir = delta > 0 ? 1 : (delta < 0 ? -1 : 0);
    int same = (dir == s->last_dir[idx]);
    s->last_dir[idx] = dir;
    if (!s->cfg.acceleration || !same) return delta;
    if (dt_ms < 16) return delta * 3;
    if (dt_ms < 26) return delta * 2;
    return delta;
}

static void emit(nc_state *s, uint8_t cc, int v) {
    if (v < 0) v = 0;
    if (v > 127) v = 127;
    if (s->midi) s->midi(s->ctx, s->mode, cc, (uint8_t)v);
}

static void handle_control(nc_state *s, uint8_t cc, uint8_t val, uint64_t now) {
    int enc = -1;
    if (cc >= NC_ENC_FIRST && cc < NC_ENC_FIRST + 8) enc = cc - NC_ENC_FIRST;
    else if (cc == NC_SPEED_DIAL) enc = 8;

    if (enc >= 0) {
        int d = accelerate(s, enc, rel_to_delta(val), now);
        if (!s->cfg.absolute_encoders) { emit(s, cc, d & 0x7F); return; } /* raw relative */
        int nv = s->enc_value[enc] + d;
        if (nv < 0) nv = 0;
        if (nv > 127) nv = 127;
        if (nv == s->enc_value[enc]) return;
        s->enc_value[enc] = nv;
        emit(s, cc, nv);
        if (enc < 8 && s->cfg.local_feedback) local_led(s, NC_LED_RING_FIRST + enc, nv);
        return;
    }
    if (cc >= NC_BUTTON_FIRST) {
        int b = cc - NC_BUTTON_FIRST;
        if (s->cfg.toggle_buttons) {
            if (!val) return;                         /* act on press only */
            s->button_state[b] = !s->button_state[b];
            emit(s, cc, s->button_state[b] ? 127 : 0);
            if (s->cfg.local_feedback) local_led(s, cc, s->button_state[b] ? 127 : 0);
        } else {
            emit(s, cc, val ? 127 : 0);
            if (s->cfg.local_feedback) local_led(s, cc, val ? 127 : 0);
        }
        return;
    }
    emit(s, cc, val);                                 /* touches etc: pass through */
}

/* original driver input (OnData) */
void nc_on_usb_data(nc_state *s, const uint8_t *p, size_t n, uint64_t now) {
    while (n > 0) {
        uint8_t b = p[0];
        if (b == 0xB0) { p += 1; n -= 1; continue; }        /* running-status byte */
        if (n < 2) break;
        if (b == 0x48) {
            s->xfader_hi = p[1] * 2;                          /* coarse part */
        } else if (b == 0x49) {
            int v8 = s->xfader_hi + (p[1] >> 6);              /* 0..255 */
            emit(s, NC_CROSSFADER, v8 >> 1);
        } else {
            handle_control(s, b, p[1], now);
        }
        p += 2; n -= 2;
    }
}

static int is_led(uint8_t cc) {
    return (cc >= NC_LED_RING_FIRST && cc < NC_LED_MODE_FIRST + 8) || cc >= NC_LED_BUTTON_FIRST;
}

void nc_on_host_cc(nc_state *s, int port, uint8_t cc, uint8_t value) {
    if (port < 0 || port >= NC_NUM_PORTS) return;
    s->shadow[port][cc] = value;
    if (port != s->mode) return;
    if (cc >= NC_LED_RING_FIRST && cc < NC_LED_RING_FIRST + 8)
        s->enc_value[cc - NC_LED_RING_FIRST] = value;     /* soft-takeover: knob follows DAW */
    if (cc >= NC_BUTTON_FIRST)
        s->button_state[cc - NC_BUTTON_FIRST] = value ? 1 : 0;
    nc_write(s, cc, value);
    nc_flush(s);
}

void nc_set_mode(nc_state *s, int port) {
    if (port < 0 || port >= NC_NUM_PORTS) return;
    int changed = port != s->mode;
    s->mode = port;
    for (int cc = 0; cc < 128; cc++) {
        if (!is_led(cc)) continue;
        int v = s->shadow[port][cc];
        if (cc >= NC_LED_MODE_FIRST && cc < NC_LED_MODE_FIRST + 8 && v < 0) continue; /* leave ring style */
        nc_write(s, (uint8_t)cc, v < 0 ? 0 : (uint8_t)v);
    }
    nc_flush(s);
    if (changed && s->mode_changed) s->mode_changed(s->ctx, port);
}

int nc_on_host_sysex(nc_state *s, int port, const uint8_t *m, size_t n) {
    (void)port;
    if (n == 8 && m[0] == 0xF0 && m[1] == 0x7D && m[2] == 0x4B && m[3] == 0x4E && m[4] == 0x43 &&
        m[5] == NC_SYSEX_SET_MODE && m[7] == 0xF7) {
        nc_set_mode(s, m[6] ? NC_PORT_NIGHTCAP : NC_PORT_MAIN);
        return 1;
    }
    return 0;
}
