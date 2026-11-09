/*
 * knocturn_core.h - Novation Nocturn USB protocol (platform independent)
 *
 * Protocol documented from the original 32-bit Novation driver software (v4.12, i386):
 *   original driver output (Initialise) / Write / Flush
 *   original driver input (OnData)
 *   original control table (Translate) / HandleEncoder
 */
#ifndef KNOCTURN_CORE_H
#define KNOCTURN_CORE_H
#include <stdint.h>
#include <stddef.h>

#define NC_DEVICE_VID 0x1235
#define NC_DEVICE_PID 0x000A

/* Hardware control numbers (as sent/received on the USB pipe) */
#define NC_ENC_FIRST        0x40  /* 8 endless encoders, relative, 0x40..0x47 */
#define NC_CROSSFADER       0x48  /* decoded from 0x48/0x49 pair */
#define NC_SPEED_DIAL       0x4A  /* relative */
#define NC_SPEED_TOUCH      0x51
#define NC_SPEED_PUSH       0x52
#define NC_XFADER_TOUCH     0x53
#define NC_ENC_TOUCH_FIRST  0x60  /* 0x60..0x67 */
#define NC_BUTTON_FIRST     0x70  /* 16 buttons 0x70..0x7F */

/* Output (LED) control numbers */
#define NC_LED_RING_FIRST   0x40  /* ring position 0..127, 0x40..0x47 */
#define NC_LED_MODE_FIRST   0x48  /* ring mode, 0x48..0x4F */
#define NC_LED_BUTTON_FIRST 0x70  /* button LEDs, 0x70..0x7F (0=off) */

#define NC_MAX_PACKET 8           /* Flush() fires when >7 bytes buffered */

/* Routing targets ("modes"): which virtual MIDI port pair owns the hardware */
#define NC_PORT_MAIN    0   /* "kNocTurn"          - DAW / Live remote script */
#define NC_PORT_NIGHTCAP 1   /* "kNocTurn NightCap"  - NightCap wrapper plug-ins */
#define NC_NUM_PORTS    2

/* Mode-switch SysEx (host -> bridge):  F0 7D 4B 4E 43 01 <port> F7
   Mode notification (bridge -> hosts):  F0 7D 4B 4E 43 02 <port> F7  (sent on both ports) */
#define NC_SYSEX_SET_MODE 0x01
#define NC_SYSEX_MODE_IS  0x02

typedef void (*nc_midi_cb)(void *ctx, int port, uint8_t cc, uint8_t value);
typedef void (*nc_mode_cb)(void *ctx, int port);
typedef void (*nc_usb_cb)(void *ctx, const uint8_t *data, size_t len);

typedef struct {
    int absolute_encoders;  /* 1: convert relative encoders to 0..127 + drive LED rings */
    int toggle_buttons;     /* 1: buttons latch on/off, LED shows state */
    int local_feedback;     /* 1: light rings/buttons locally */
    int acceleration;       /* 1: speed-sensitive encoders (like NightCap) */
} nc_config;

typedef struct {
    nc_config cfg;
    /* input side */
    int xfader_hi;                /* original driver input +0x94 */
    int enc_value[9];             /* 8 encoders + speed dial (absolute mode) */
    int last_dir[9];
    uint64_t last_time[9];
    uint8_t button_state[16];
    /* output side (original driver output) */
    uint8_t out_buf[NC_MAX_PACKET];
    size_t out_len;
    int out_cache[128];           /* last value sent per control, -1 = unknown */
    /* routing */
    int mode;                     /* NC_PORT_* that currently owns the hardware */
    int shadow[NC_NUM_PORTS][128];/* LED state requested by each port, -1 = never set */
    nc_usb_cb usb;
    nc_midi_cb midi;
    nc_mode_cb mode_changed;
    void *ctx;
} nc_state;

void nc_init(nc_state *s, const nc_config *cfg, nc_usb_cb usb, nc_midi_cb midi, void *ctx);
/* Sends the device init sequence (call right after opening the pipe). */
void nc_send_init(nc_state *s);
/* Feed raw bytes read from the interrupt IN pipe. now_ns = monotonic clock. */
void nc_on_usb_data(nc_state *s, const uint8_t *buf, size_t len, uint64_t now_ns);
/* Feed a CC received from a host port (LED feedback). Only the active port reaches the hardware;
   the others are remembered and restored when that port becomes active. */
void nc_on_host_cc(nc_state *s, int port, uint8_t cc, uint8_t value);
/* Feed a complete SysEx message received from a host port. Returns 1 if it was a bridge command. */
int nc_on_host_sysex(nc_state *s, int port, const uint8_t *msg, size_t len);
/* Switch which port owns the hardware; restores that port's LEDs. */
void nc_set_mode(nc_state *s, int port);
/* Queue/flush raw control writes to the device. */
void nc_write(nc_state *s, uint8_t cc, uint8_t value);
void nc_flush(nc_state *s);
/* Forget the LED cache (e.g. after reconnect) */
void nc_reset_cache(nc_state *s);
/* Turn all LEDs off */
void nc_clear_leds(nc_state *s);
#endif
